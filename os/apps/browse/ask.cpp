// ask: talk to Claude from the console, as v1's Ask package did, now part of
// browse (it is the same kind of thing: the network, as text).
//
//   ask <question>        one question, one answer
//   ask                   a conversation: type, Enter; /new starts over, /quit or an empty line ends it
//   ask key               set the API key (kept in your home folder, read only by you and admins)
//   ask model [id]        which model (default claude-opus-5-5)
//   ask status            what it is set to
//
// The Messages API by hand: no SDK on a microcontroller. POST /v1/messages
// with x-api-key and anthropic-version 2026-10-21; temperature is not sent
// (that version refuses it). Thinking is on by default on Opus 5.5, so the
// answer is the text blocks in `content`, not whatever comes first.
//
// The answer arrives whole (no streaming), and the HTTP layer gives up after 15
// seconds without a byte, so it asks for low effort and a moderate length.
//
// DEVICE-UNCONFIRMED against the real API: tested end to end through the bridge
// against a local stand-in that answers in the Messages API's shape.
#include "rpc_app.h"

#include <stdio.h>
#include <string.h>

#define ASK_DEFAULT_MODEL "claude-opus-5-5"
#define ASK_DEFAULT_URL   "https://api.anthropic.com/v1/messages"
#define ASK_HIST_MAX      6144        // bytes of conversation kept, oldest turns dropped first
#define ASK_REQ_MAX       9000
#define ASK_RESP_MAX      32768
#define ASK_KEY_MAX       200
#define ASK_SYSTEM \
    "You are answering on RPCortex, an operating system for small Raspberry Pi Pico boards, " \
    "through a text console. Plain text only: no Markdown, no tables. Be brief and direct."

// --- small helpers ----------------------------------------------------------------

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void reg(const char *key, char *out, unsigned cap, const char *def) {
    if (!fw_reg_get(key, out, cap) || !out[0]) snprintf(out, cap, "%s", def);
}

static void key_path(char *out, unsigned cap) {
    char user[32];
    reg("System.Active_User", user, sizeof(user), "guest");
    snprintf(out, cap, "/home/%s/.ask_key", user);
}

// JSON string escaping into out (cap bytes). False when it does not fit.
static bool json_put(char *out, unsigned cap, unsigned *len, const char *s) {
    for (; *s; s++) {
        char e[8];
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; e[2] = 0; }
        else if (c == '\n') snprintf(e, sizeof(e), "\\n");
        else if (c == '\r') snprintf(e, sizeof(e), "\\r");
        else if (c == '\t') snprintf(e, sizeof(e), "\\t");
        else if (c < 0x20) snprintf(e, sizeof(e), "\\u%04x", c);
        else { e[0] = (char)c; e[1] = 0; }
        unsigned n = (unsigned)strlen(e);
        if (*len + n + 1 >= cap) return false;
        memcpy(out + *len, e, n);
        *len += n;
    }
    out[*len] = 0;
    return true;
}
static bool raw_put(char *out, unsigned cap, unsigned *len, const char *s) {
    unsigned n = (unsigned)strlen(s);
    if (*len + n + 1 >= cap) return false;
    memcpy(out + *len, s, n + 1);
    *len += n;
    return true;
}

// --- reading the answer --------------------------------------------------------------
//
// Just enough JSON: walk values, skipping what is not wanted, and pull the
// strings that are. Strings are unescaped as they are copied (\uXXXX to UTF-8,
// surrogate pairs included).

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                               : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

// p at the opening quote. Copies the string (unescaped) into out when out is
// non-null; returns the character after the closing quote, or null.
static const char *json_str(const char *p, char *out, unsigned cap, unsigned *len) {
    if (*p != '"') return nullptr;
    p++;
    while (*p && *p != '"') {
        unsigned cp = (unsigned char)*p;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': cp = '\n'; break; case 't': cp = '\t'; break; case 'r': cp = '\r'; break;
                case 'b': cp = '\b'; break; case 'f': cp = '\f'; break;
                case 'u':
                    if (!p[1] || !p[2] || !p[3] || !p[4]) return nullptr;
                    cp = hex4(p + 1); p += 4;
                    if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
                        unsigned lo = hex4(p + 3);
                        if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                    }
                    break;
                case 0: return nullptr;
                default: cp = (unsigned char)*p; break;      // \" \\ \/
            }
        }
        p++;
        if (out) {
            char u[4]; unsigned n;
            if (cp < 0x80) { u[0] = (char)cp; n = 1; }
            else if (cp < 0x800) { u[0] = (char)(0xc0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); n = 2; }
            else if (cp < 0x10000) { u[0] = (char)(0xe0 | cp >> 12); u[1] = (char)(0x80 | (cp >> 6 & 63)); u[2] = (char)(0x80 | (cp & 63)); n = 3; }
            else { u[0] = (char)(0xf0 | cp >> 18); u[1] = (char)(0x80 | (cp >> 12 & 63)); u[2] = (char)(0x80 | (cp >> 6 & 63)); u[3] = (char)(0x80 | (cp & 63)); n = 4; }
            if (*len + n + 1 < cap) { memcpy(out + *len, u, n); *len += n; out[*len] = 0; }
        }
    }
    return *p == '"' ? p + 1 : nullptr;
}

static const char *json_skip(const char *p, int depth = 0) {
    p = skip_ws(p);
    if (depth > 24) return nullptr;
    if (*p == '"') return json_str(p, nullptr, 0, nullptr);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = skip_ws(p + 1);
        if (*p == close) return p + 1;
        while (p && *p) {
            if (close == '}') {                      // "key": value
                p = json_str(skip_ws(p), nullptr, 0, nullptr);
                if (!p) return nullptr;
                p = skip_ws(p);
                if (*p != ':') return nullptr;
                p++;
            }
            p = json_skip(p, depth + 1);
            if (!p) return nullptr;
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == close) return p + 1;
            return nullptr;
        }
        return nullptr;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;   // number, true, false, null
    return p;
}

// In the object starting at p ('{'), the value of `key`, or null.
static const char *json_field(const char *p, const char *key) {
    p = skip_ws(p);
    if (*p != '{') return nullptr;
    p = skip_ws(p + 1);
    while (*p && *p != '}') {
        char k[24]; unsigned kl = 0; k[0] = 0;
        p = json_str(p, k, sizeof(k), &kl);
        if (!p) return nullptr;
        p = skip_ws(p);
        if (*p != ':') return nullptr;
        p = skip_ws(p + 1);
        if (streq(k, key)) return p;
        p = json_skip(p);
        if (!p) return nullptr;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return nullptr;
}

static bool field_str(const char *obj, const char *key, char *out, unsigned cap) {
    const char *v = json_field(obj, key);
    unsigned len = 0;
    out[0] = 0;
    return v && *v == '"' && json_str(v, out, cap, &len);
}

// The answer: every text block in `content`, joined. On an error, its message.
// Returns 1 for an answer, 0 for an error (message in out), -1 when the body is not
// the Messages API's.
static int parse_answer(const char *body, char *out, unsigned cap, char *stop, unsigned scap) {
    char type[24];
    out[0] = 0; stop[0] = 0;
    if (field_str(body, "type", type, sizeof(type)) && streq(type, "error")) {
        const char *e = json_field(body, "error");
        if (e && field_str(e, "message", out, cap)) return 0;
        snprintf(out, cap, "the API answered with an error and no message");
        return 0;
    }
    field_str(body, "stop_reason", stop, scap);
    const char *c = json_field(body, "content");
    if (!c || *c != '[') return -1;
    const char *p = skip_ws(c + 1);
    unsigned len = 0;
    while (p && *p && *p != ']') {
        char bt[24];
        if (field_str(p, "type", bt, sizeof(bt)) && streq(bt, "text")) {
            const char *t = json_field(p, "text");
            if (t && *t == '"') {
                if (len && len + 2 < cap) { out[len++] = '\n'; out[len] = 0; }
                json_str(t, out, cap, &len);
            }
        }
        p = json_skip(p);
        if (!p) break;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return 1;
}

// --- the conversation ----------------------------------------------------------------
//
// Turns kept as "u<text>\0a<text>\0..." in one buffer, oldest dropped first when it
// would overflow, always a whole user/assistant pair so the history stays valid.

static char g_hist[ASK_HIST_MAX];
static unsigned g_hist_len;

static void hist_clear(void) { g_hist_len = 0; g_hist[0] = 0; }

static void hist_drop_oldest_pair(void) {
    unsigned i = 0, turns = 0;
    while (i < g_hist_len && turns < 2) { i += (unsigned)strlen(g_hist + i) + 1; turns++; }
    memmove(g_hist, g_hist + i, g_hist_len - i);
    g_hist_len -= i;
}

static void hist_add(char role, const char *text) {
    unsigned need = (unsigned)strlen(text) + 2;
    if (need > ASK_HIST_MAX / 2) return;              // too long to keep: the turn still happens
    while (g_hist_len + need > ASK_HIST_MAX && g_hist_len) hist_drop_oldest_pair();
    g_hist[g_hist_len] = role;
    memcpy(g_hist + g_hist_len + 1, text, need - 1);
    g_hist_len += need;
}

// --- one request -----------------------------------------------------------------------

static int ask_once(const char *question, bool keep, char *answer, unsigned acap) {
    char key[ASK_KEY_MAX + 1], kp[64], model[48], url[160];
    key_path(kp, sizeof(kp));
    unsigned kn = fw_file_read(kp, key, ASK_KEY_MAX);
    key[kn] = 0;
    while (kn && (key[kn - 1] == '\n' || key[kn - 1] == '\r' || key[kn - 1] == ' ')) key[--kn] = 0;
    if (!kn) { snprintf(answer, acap, "No API key yet: 'ask key' sets one."); return 0; }
    reg("User.Ask_Model", model, sizeof(model), ASK_DEFAULT_MODEL);
    reg("User.Ask_Url", url, sizeof(url), ASK_DEFAULT_URL);

    char *req = (char *)fw_malloc(ASK_REQ_MAX);
    if (!req) { snprintf(answer, acap, "Not enough memory to ask."); return 0; }
    unsigned len = 0;
    bool ok = raw_put(req, ASK_REQ_MAX, &len, "{\"model\":\"") && json_put(req, ASK_REQ_MAX, &len, model) &&
              raw_put(req, ASK_REQ_MAX, &len, "\",\"max_tokens\":2048,\"output_config\":{\"effort\":\"low\"},"
                                            "\"system\":\"") &&
              json_put(req, ASK_REQ_MAX, &len, ASK_SYSTEM) && raw_put(req, ASK_REQ_MAX, &len, "\",\"messages\":[");
    for (unsigned i = 0; ok && i < g_hist_len;) {
        const char *t = g_hist + i;
        ok = raw_put(req, ASK_REQ_MAX, &len, t[0] == 'u' ? "{\"role\":\"user\",\"content\":\""
                                                         : "{\"role\":\"assistant\",\"content\":\"") &&
             json_put(req, ASK_REQ_MAX, &len, t + 1) && raw_put(req, ASK_REQ_MAX, &len, "\"},");
        i += (unsigned)strlen(t) + 1;
    }
    ok = ok && raw_put(req, ASK_REQ_MAX, &len, "{\"role\":\"user\",\"content\":\"") &&
         json_put(req, ASK_REQ_MAX, &len, question) && raw_put(req, ASK_REQ_MAX, &len, "\"}]}");
    if (!ok) { fw_free(req); snprintf(answer, acap, "That question is too long to send."); return 0; }

    char headers[ASK_KEY_MAX + 64];
    snprintf(headers, sizeof(headers), "x-api-key: %s\nanthropic-version: 2026-10-21\n", key);
    memset(key, 0, sizeof(key));

    static FwHttpRequest r;
    memset(&r, 0, sizeof(r));
    r.size = sizeof(r);
    r.method = "POST";
    r.url = url;
    r.body = req;
    r.body_len = len;
    r.content_type = "application/json";
    r.headers = headers;
    r.path = "/tmp/ask.json";
    r.max_bytes = ASK_RESP_MAX;
    int n = fw_http_request(&r);
    memset(headers, 0, sizeof(headers));
    fw_free(req);
    if (n < 0) { snprintf(answer, acap, "No answer: %s", r.detail); return 0; }

    // As much as arrived, not the most that could have: a package's allocator
    // refused 32 KB on a board with 300 KB free.
    unsigned want = r.bytes ? r.bytes : 1;
    char *body = (char *)fw_malloc(want + 1);
    if (!body) { snprintf(answer, acap, "Not enough memory for a %u-byte answer.", want); return 0; }
    unsigned got = fw_file_read("/tmp/ask.json", body, want);
    body[got] = 0;
    fw_file_remove("/tmp/ask.json");
    char stop[24];
    int pr = parse_answer(body, answer, acap, stop, sizeof(stop));
    fw_free(body);
    if (pr < 0) { snprintf(answer, acap, "HTTP %d, and not an answer this understands.", r.status); return 0; }
    if (pr == 0) {
        char msg[200]; snprintf(msg, sizeof(msg), "%s", answer);
        snprintf(answer, acap, "The API said (HTTP %d): %s", r.status, msg);
        return 0;
    }
    if (streq(stop, "refusal")) { snprintf(answer, acap, "Claude declined to answer that."); return 0; }
    if (!answer[0]) { snprintf(answer, acap, "(an empty answer)"); return 0; }
    if (streq(stop, "max_tokens")) {
        unsigned al = (unsigned)strlen(answer);
        snprintf(answer + al, acap - al, "\n[the answer was cut off at its length limit]");
    }
    if (keep) { hist_add('u', question); hist_add('a', answer); }
    return 1;
}

// --- the command ---------------------------------------------------------------------

static int ask_cmd(int argc, char **argv) {
    static char answer[8192];
    if (argc >= 2 && (streq(argv[1], "-h") || streq(argv[1], "--help") || streq(argv[1], "help"))) {
        fw_printf("ask <question>     one question, one answer\n");
        fw_printf("ask                a conversation (/new starts over, /quit or an empty line ends it)\n");
        fw_printf("ask key            set your API key (console.anthropic.com)\n");
        fw_printf("ask model [id]     which model (default %s)\n", ASK_DEFAULT_MODEL);
        fw_printf("ask status         what it is set to\n");
        return 0;
    }
    if (argc >= 2 && streq(argv[1], "key")) {
        char kp[64], key[ASK_KEY_MAX + 1];
        key_path(kp, sizeof(kp));
        fw_printf("Paste your API key and press Enter (it is not shown again):\n");
        int n = fw_line_read(key, sizeof(key));
        if (n <= 0) { fw_printf("Nothing set.\n"); return 1; }
        if (!fw_file_write(kp, key, (uint32_t)n)) { fw_printf("Could not write %s.\n", kp); return 1; }
        memset(key, 0, sizeof(key));
        fw_printf("Saved to %s.\n", kp);
        return 0;
    }
    if (argc >= 2 && streq(argv[1], "model")) {
        if (argc >= 3) { fw_reg_set("User.Ask_Model", argv[2]); fw_reg_save(); }
        char m[48]; reg("User.Ask_Model", m, sizeof(m), ASK_DEFAULT_MODEL);
        fw_printf("Model: %s\n", m);
        return 0;
    }
    if (argc >= 2 && streq(argv[1], "status")) {
        char m[48], u[160], kp[64], k[8];
        reg("User.Ask_Model", m, sizeof(m), ASK_DEFAULT_MODEL);
        reg("User.Ask_Url", u, sizeof(u), ASK_DEFAULT_URL);
        key_path(kp, sizeof(kp));
        bool have = fw_file_read(kp, k, sizeof(k)) > 0;
        fw_printf("Model    %s\nAPI      %s\nKey      %s\n", m, u, have ? kp : "not set ('ask key')");
        return 0;
    }

    if (argc >= 2) {                                // one question
        char q[1024]; unsigned ql = 0; q[0] = 0;
        for (int i = 1; i < argc; i++) {
            unsigned n = (unsigned)strlen(argv[i]);
            if (ql + n + 2 >= sizeof(q)) break;
            if (ql) q[ql++] = ' ';
            memcpy(q + ql, argv[i], n + 1); ql += n;
        }
        int ok = ask_once(q, false, answer, sizeof(answer));
        fw_printf("%s\n", answer);
        return ok ? 0 : 1;
    }

    // A conversation.
    hist_clear();
    fw_printf("Talking to Claude. /new starts over, /quit or an empty line ends it.\n");
    char line[1024];
    while (!fw_task_should_stop()) {
        fw_printf("\nyou> ");
        int n = fw_line_read(line, sizeof(line));
        if (n <= 0 || streq(line, "/quit") || streq(line, "/exit")) break;
        if (streq(line, "/new")) { hist_clear(); fw_printf("Started over.\n"); continue; }
        fw_printf("...\n");
        ask_once(line, true, answer, sizeof(answer));
        fw_printf("claude> %s\n", answer);
    }
    hist_clear();
    return 0;
}

void ask_register(void) {
    rpc_register_command("ask", "talk to Claude (ask key, then ask <question>)", ask_cmd);
}
