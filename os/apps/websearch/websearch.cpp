// WebSearch — look things up from the shell.
//
//   search <query>      an instant answer, or the closest Wikipedia articles
//   search -w <query>   skip the instant answer, list articles
//   wiki <topic>        a one-paragraph summary
//
// Three public, key-free endpoints: DuckDuckGo's Instant Answer API, and
// Wikipedia's OpenSearch and REST summary.
//
// --- what changed from v1 ---------------------------------------------------
//
// v1 shipped with a warning that the certificates were not checked, because the
// device had no roots to check them against. It does now: fw_http_get verifies
// against the bundle in /os/ca.pem and REFUSES a connection it cannot verify
// rather than quietly downgrading. So the warning is gone, and with it the
// reason to treat these lookups as untrusted.
//
// The other difference is the memory. A sandboxed package gets a 12 KB arena,
// which is the whole budget for the response, the decoded text and the working
// buffers at once — so this reads one reply at a time into a single block and
// never holds two.
#include "rpc_app.h"

RPC_APP_VER("websearch", "2.1");

// One response at a time, and never more than this. Wikipedia summaries run to
// two or three kilobytes and DuckDuckGo's answers to rather more; anything that
// does not fit is truncated at a character boundary rather than refused, since
// a clipped answer still answers.
#define REPLY_MAX   6144
// Long enough for a percent-encoded query plus the longest endpoint.
#define URL_MAX      512
// An extract or an instant answer. Titles are much shorter and get their own
// size, because four of them at the size of a paragraph is most of the budget.
#define FIELD_MAX    640
#define TITLE_MAX    128
#define TITLE_N        4

// NOT ON THE STACK, and that is the whole point.
//
// A package gets three kilobytes of stack before the firmware's own reserve —
// and an ABI call does not switch stacks, so fw_http_get runs the TLS handshake
// and certificate verification on THIS one. A frame holding two kilobytes of
// buffers that then calls out to HTTPS is the difference between working and
// faulting somewhere inside mbedtls, which is nobody's idea of a clue.
//
// One command runs at a time, so sharing these is safe and costs .bss the
// package was going to need for the titles anyway.
static char g_url[URL_MAX];
static char g_query[URL_MAX];
static char g_field[FIELD_MAX];
static char g_titles[TITLE_N][TITLE_MAX];

// --- small string helpers ----------------------------------------------------

static unsigned s_len(const char *s) {
    unsigned n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool s_eq_n(const char *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

// Append to a bounded buffer, always leaving it terminated. Returns false when
// it would not fit, so a caller can stop rather than emit a truncated URL — a
// half-encoded query is a request for the wrong thing, not a shorter one.
static bool s_add(char *dst, unsigned cap, unsigned *len, const char *src) {
    unsigned n = s_len(src);
    if (*len + n + 1 > cap) return false;
    for (unsigned i = 0; i < n; i++) dst[*len + i] = src[i];
    *len += n;
    dst[*len] = 0;
    return true;
}

// --- percent-encoding --------------------------------------------------------
//
// Unreserved characters pass through; everything else becomes %XX. Written out
// by hand because the set is small and the alternative is trusting a locale.
static bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

static bool url_encode(char *dst, unsigned cap, unsigned *len, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (is_unreserved((char)c)) {
            if (*len + 2 > cap) return false;
            dst[(*len)++] = (char)c;
        } else {
            if (*len + 4 > cap) return false;
            dst[(*len)++] = '%';
            dst[(*len)++] = hex[c >> 4];
            dst[(*len)++] = hex[c & 0x0F];
        }
        dst[*len] = 0;
    }
    return true;
}

// Join the remaining arguments into one space-separated query.
static bool build_query(char *dst, unsigned cap, unsigned *len,
                        int argc, char **argv, int first) {
    for (int i = first; i < argc; i++) {
        if (i > first && !url_encode(dst, cap, len, " ")) return false;
        if (!url_encode(dst, cap, len, argv[i])) return false;
    }
    return *len > 0;
}

// --- just enough JSON --------------------------------------------------------
//
// These replies are machine-generated and shallow, so a full parser would cost
// more arena than the response it was parsing. What is needed is "the string
// after this key" and "the strings in this array", both of which are a scan.
//
// The one thing that CANNOT be skipped is unescaping. Wikipedia sends \" and
// \\ and \uXXXX, and treating those literally puts backslashes through the
// middle of every other summary.

// Decode a JSON string body starting AFTER its opening quote. Returns the index
// just past the closing quote, or 0 if it never closed.
static unsigned json_string_at(const char *j, unsigned i, char *out, unsigned cap) {
    unsigned o = 0;
    while (j[i]) {
        char c = j[i];
        if (c == '"') { if (o < cap) out[o] = 0; else out[cap - 1] = 0; return i + 1; }
        if (c == '\\') {
            i++;
            char e = j[i];
            if (!e) break;
            char sub = 0;
            switch (e) {
            case 'n': sub = '\n'; break;
            case 't': sub = ' ';  break;      // tabs would wreck the wrapping
            case 'r': i++; continue;          // dropped: \r\n becomes one break
            case 'b': case 'f': i++; continue;
            case 'u': {
                // \uXXXX. Anything outside plain ASCII becomes '?' rather than
                // mangled UTF-8: this is a serial terminal, and a wrong byte
                // sequence there desynchronises the whole line.
                unsigned v = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = j[i + k];
                    if (!h) return 0;
                    v <<= 4;
                    if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else return 0;
                }
                i += 5;
                sub = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
                if (o + 1 < cap) out[o++] = sub;
                continue;
            }
            default: sub = e; break;          // \" \\ \/ and anything else
            }
            if (o + 1 < cap) out[o++] = sub;
            i++;
            continue;
        }
        if (o + 1 < cap) out[o++] = c;
        i++;
    }
    return 0;                                  // unterminated
}

// Find "key" at the top level-ish and decode the string that follows it.
//
// Deliberately not a real path lookup: these replies put the fields wanted here
// at the top, and the first match is the right one. A nested key of the same
// name would be found instead, which is why each caller asks for a key that
// only appears once in the reply it is reading.
static bool json_get(const char *j, const char *key, char *out, unsigned cap) {
    unsigned klen = s_len(key);
    for (unsigned i = 0; j[i]; i++) {
        if (j[i] != '"') continue;
        if (!s_eq_n(j + i + 1, key, klen)) continue;
        if (j[i + 1 + klen] != '"') continue;
        unsigned p = i + 2 + klen;
        while (j[p] == ' ') p++;
        if (j[p] != ':') continue;
        p++;
        while (j[p] == ' ') p++;
        if (j[p] != '"') return false;         // present but not a string
        return json_string_at(j, p + 1, out, cap) != 0;
    }
    return false;
}

// --- output ------------------------------------------------------------------

// Wrap at a fixed width on word boundaries. Fixed rather than measured because
// a package cannot ask the terminal how wide it is, and 72 reads correctly in
// every terminal wide enough to run the shell at all.
#define WRAP_COLS 72

// ONE CALL PER LINE, not one per character.
//
// The first version printed each character with its own fw_printf, and every
// one of those is a supervisor call: an exception in, the firmware's vsnprintf
// running on the package's own stack, and an exception out, to move one byte.
// A three-hundred-character summary cost three hundred round trips across the
// sandbox boundary — slow enough to watch, and three hundred separate moments
// of being deep inside the firmware.
//
// The line is built here and handed over whole.
static void print_wrapped(const char *text, const char *indent) {
    char line[WRAP_COLS + 8];
    unsigned col = 0, i = 0;

    while (text[i]) {
        if (text[i] == '\n') {
            line[col] = 0;
            fw_printf("%s%s\n", indent, line);
            col = 0;
            i++;
            while (text[i] == ' ') i++;        // no leading space on a new line
            continue;
        }
        // Measure the next word so it can be moved down whole.
        unsigned w = 0;
        while (text[i + w] && text[i + w] != ' ' && text[i + w] != '\n') w++;

        // A word wider than the whole column cannot be wrapped anywhere, so it
        // is broken rather than allowed to run off the end of `line`.
        if (w > WRAP_COLS) w = WRAP_COLS;

        if (col && col + 1 + w > WRAP_COLS) {
            line[col] = 0;
            fw_printf("%s%s\n", indent, line);
            col = 0;
        } else if (col) {
            line[col++] = ' ';
        }
        for (unsigned k = 0; k < w; k++) line[col++] = text[i + k];
        i += w;
        while (text[i] == ' ') i++;
    }
    line[col] = 0;
    fw_printf("%s%s\n", indent, line);
}

// --- fetching ----------------------------------------------------------------

static bool online(void) {
    if (fw_net_connected()) return true;
    fw_printf("Not connected. Use 'wifi connect <ssid>' first.\n");
    return false;
}

// Fetch into `buf`, terminate it, and report anything that went wrong in the
// caller's words rather than as a number.
static bool fetch(const char *url, char *buf, unsigned cap) {
    int n = fw_http_get(url, buf, cap - 1);
    if (n < 0) {
        fw_printf("The request failed. The site may be unreachable, or its\n");
        fw_printf("certificate could not be verified against /os/ca.pem.\n");
        return false;
    }
    buf[n] = 0;
    if (n == 0) {
        fw_printf("The server sent an empty reply.\n");
        return false;
    }
    return true;
}

// --- Wikipedia ---------------------------------------------------------------

// OpenSearch returns ["query", ["title", ...], ["desc", ...], ["url", ...]].
// The titles are the second array, so the scan skips the first bracket pair and
// then reads strings until the array closes.
//
// The query string it skips over is decoded into the first title slot and then
// overwritten, rather than into a buffer of its own — this runs just before an
// HTTPS fetch, and a spare 640 bytes of frame here is 640 bytes the TLS
// handshake does not get.
static int wiki_titles(const char *json, char titles[][TITLE_MAX], int max) {
    if (max < 1) return 0;
    unsigned i = 0;
    // Past the query string: the first '[' then the first '"..."' pair.
    while (json[i] && json[i] != '[') i++;
    if (!json[i]) return 0;
    i++;
    while (json[i] && json[i] != '"') i++;
    if (!json[i]) return 0;
    i = json_string_at(json, i + 1, titles[0], TITLE_MAX);
    if (!i) return 0;
    // Now the titles array.
    while (json[i] && json[i] != '[') i++;
    if (!json[i]) return 0;
    i++;
    int n = 0;
    while (json[i] && json[i] != ']' && n < max) {
        while (json[i] == ' ' || json[i] == ',') i++;
        if (json[i] != '"') break;
        i = json_string_at(json, i + 1, titles[n], TITLE_MAX);
        if (!i) break;
        n++;
    }
    // Nothing matched: slot 0 still holds the query that was skipped over, and
    // printing that back as a result is exactly the wrong answer.
    if (n == 0) titles[0][0] = 0;
    return n;
}

static int wiki_summary(const char *topic_encoded, char *buf, unsigned cap) {
    unsigned len = 0;
    g_url[0] = 0;
    if (!s_add(g_url, URL_MAX, &len, "https://en.wikipedia.org/api/rest_v1/page/summary/") ||
        !s_add(g_url, URL_MAX, &len, topic_encoded)) {
        fw_printf("That topic is too long.\n");
        return 1;
    }
    if (!fetch(g_url, buf, cap)) return 1;

    if (!json_get(buf, "extract", g_field, FIELD_MAX)) {
        // The REST endpoint answers 404 with a JSON body carrying a title.
        if (json_get(buf, "title", g_field, FIELD_MAX))
            fw_printf("No article for that. (%s)\n", g_field);
        else
            fw_printf("No article for that.\n");
        return 1;
    }
    // The title is read into a title slot rather than a second field buffer,
    // and printed before the extract is touched again.
    if (json_get(buf, "title", g_titles[0], TITLE_MAX)) fw_printf("\n  %s\n\n", g_titles[0]);
    else                                                fw_printf("\n");
    print_wrapped(g_field, "  ");
    fw_printf("\n");
    return 0;
}

static int wiki_list(const char *query_encoded, char *buf, unsigned cap) {
    unsigned len = 0;
    g_url[0] = 0;
    if (!s_add(g_url, URL_MAX, &len, "https://en.wikipedia.org/w/api.php?action=opensearch&limit=8&format=json&search=") ||
        !s_add(g_url, URL_MAX, &len, query_encoded)) {
        fw_printf("That query is too long.\n");
        return 1;
    }
    if (!fetch(g_url, buf, cap)) return 1;

    // Four, not the eight asked for: four is enough to choose from, and the
    // slots are held for the life of the package rather than borrowed.
    int n = wiki_titles(buf, g_titles, TITLE_N);
    if (n <= 0) {
        fw_printf("Nothing matched.\n");
        return 1;
    }
    fw_printf("\n  Articles:\n");
    for (int i = 0; i < n; i++) fw_printf("    %s\n", g_titles[i]);
    fw_printf("\n  'wiki <title>' reads one.\n\n");
    return 0;
}

// --- the instant answer ------------------------------------------------------

static int instant_answer(const char *query_encoded, char *buf, unsigned cap,
                          bool *answered) {
    *answered = false;
    unsigned len = 0;
    g_url[0] = 0;
    if (!s_add(g_url, URL_MAX, &len, "https://api.duckduckgo.com/?format=json&no_html=1&skip_disambig=1&q=") ||
        !s_add(g_url, URL_MAX, &len, query_encoded)) {
        fw_printf("That query is too long.\n");
        return 1;
    }
    if (!fetch(g_url, buf, cap)) return 1;

    // AbstractText is the prose answer; Answer is the computed one (a sum, a
    // conversion). Either counts, neither is guaranteed.
    if (json_get(buf, "Answer", g_field, FIELD_MAX) && g_field[0]) {
        fw_printf("\n");
        print_wrapped(g_field, "  ");
        fw_printf("\n");
        *answered = true;
        return 0;
    }
    if (json_get(buf, "AbstractText", g_field, FIELD_MAX) && g_field[0]) {
        fw_printf("\n");
        print_wrapped(g_field, "  ");
        // The source goes into a title slot, AFTER the extract has been
        // printed — g_field is the only field buffer and it is still holding
        // the answer until this point.
        if (json_get(buf, "AbstractSource", g_titles[0], TITLE_MAX) && g_titles[0][0])
            fw_printf("\n  - %s\n", g_titles[0]);
        fw_printf("\n");
        *answered = true;
        return 0;
    }
    return 0;                                  // no answer, and that is not an error
}

// --- commands ----------------------------------------------------------------

static void usage(void) {
    fw_printf("Usage:\n");
    fw_printf("  search <query>       an instant answer, or matching articles\n");
    fw_printf("  search -w <query>    skip the answer, list articles\n");
    fw_printf("  wiki <topic>         a one-paragraph summary\n");
}

static int search_cmd(int argc, char **argv) {
    int first = 1;
    bool wiki_only = false;
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'w' && !argv[1][2]) {
        wiki_only = true;
        first = 2;
    }
    if (argc <= first) { usage(); return 1; }
    if (!online()) return 1;

    // Static, like the rest: this frame is live across two HTTPS fetches.
    unsigned qlen = 0;
    g_query[0] = 0;
    if (!build_query(g_query, URL_MAX, &qlen, argc, argv, first)) {
        fw_printf("That query is too long.\n");
        return 1;
    }

    char *buf = (char *)fw_malloc(REPLY_MAX);
    if (!buf) { fw_printf("Not enough memory for the reply.\n"); return 1; }

    int rc = 0;
    if (!wiki_only) {
        bool answered = false;
        rc = instant_answer(g_query, buf, REPLY_MAX, &answered);
        if (rc == 0 && answered) { fw_free(buf); return 0; }
        if (rc == 0) fw_printf("\n  No direct answer. Looking for articles...\n");
    }
    // Either asked for, or nothing better was found.
    if (rc == 0) rc = wiki_list(g_query, buf, REPLY_MAX);
    fw_free(buf);
    return rc;
}

static int wiki_cmd(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (!online()) return 1;

    // Underscores, not %20: the REST path wants a page title, and a title with
    // a space in it is written with an underscore there.
    unsigned len = 0;
    g_query[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && !url_encode(g_query, URL_MAX, &len, "_")) break;
        if (!url_encode(g_query, URL_MAX, &len, argv[i])) {
            fw_printf("That topic is too long.\n");
            return 1;
        }
    }
    if (!len) { usage(); return 1; }

    char *buf = (char *)fw_malloc(REPLY_MAX);
    if (!buf) { fw_printf("Not enough memory for the reply.\n"); return 1; }
    int rc = wiki_summary(g_query, buf, REPLY_MAX);
    fw_free(buf);
    return rc;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("search", "look something up over the network", search_cmd);
    rpc_register_command("wiki", "read a one-paragraph summary", wiki_cmd);
    return 0;
}
