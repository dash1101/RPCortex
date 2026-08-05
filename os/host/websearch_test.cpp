// websearch_test — the parts of a network package that fail silently.
//
// A wrong URL encoding asks a public API for the wrong thing and gets a polite
// answer about something else. A JSON scanner that stops at the wrong quote
// prints half a sentence, or the value belonging to the next key. Neither
// raises anything, on the host or on a board, so neither is caught by running
// the command and seeing text appear.
//
// So the package's source is compiled here directly, with the ABI stubbed, and
// its scanners are pointed at replies shaped like the real ones.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../include/rpc_app.h"

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

// --- the ABI, enough of it to link ------------------------------------------
//
// fw_printf collects into a buffer so the wrapper can be checked as text
// instead of by eye.
static char g_out[8192];
static unsigned g_out_len;

extern "C" {
int fw_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + g_out_len, sizeof(g_out) - g_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_out_len += (unsigned)n;
    return n;
}
void *fw_malloc(size_t n)  { return malloc(n); }
void  fw_free(void *p)     { free(p); }
int   fw_net_connected(void) { return 1; }
int   fw_http_get(const char *, void *, unsigned) { return -1; }
int   rpc_register_command(const char *, const char *, RpcCommandFn) { return 1; }
}

// The package itself. Its helpers are static, which is exactly why the source is
// included rather than linked: they are the things worth testing.
#include "../apps/websearch/websearch.cpp"

static void reset_out(void) { g_out_len = 0; g_out[0] = 0; }

int main(void) {
    printf("websearch_test - encoding and reply parsing\n");

    // --- percent-encoding ---------------------------------------------------
    {
        char b[128]; unsigned n = 0; b[0] = 0;
        ck(url_encode(b, sizeof(b), &n, "hello"), "a plain word encodes");
        ck(strcmp(b, "hello") == 0, "and is unchanged");

        n = 0; b[0] = 0;
        url_encode(b, sizeof(b), &n, "a b");
        ck(strcmp(b, "a%20b") == 0, "a space becomes %20, never a plus");

        n = 0; b[0] = 0;
        url_encode(b, sizeof(b), &n, "C++ & co/?=#");
        ck(strcmp(b, "C%2B%2B%20%26%20co%2F%3F%3D%23") == 0,
           "every reserved character is escaped");

        n = 0; b[0] = 0;
        url_encode(b, sizeof(b), &n, "-_.~");
        ck(strcmp(b, "-_.~") == 0, "the unreserved set passes through");

        // High bytes must not sign-extend into a negative index.
        n = 0; b[0] = 0;
        url_encode(b, sizeof(b), &n, "\xC3\xA9");
        ck(strcmp(b, "%C3%A9") == 0, "a UTF-8 byte encodes as two hex digits");

        // A query that will not fit is refused, not truncated: a shortened
        // query is a request for something else.
        char small[8]; unsigned m = 0; small[0] = 0;
        ck(!url_encode(small, sizeof(small), &m, "far too long to fit in here"),
           "an overlong query is refused rather than clipped");
    }

    // --- JSON strings -------------------------------------------------------
    {
        char v[256];
        const char *j = "{\"title\":\"Pico\",\"extract\":\"A microcontroller.\"}";
        ck(json_get(j, "title", v, sizeof(v)) && strcmp(v, "Pico") == 0,
           "a plain field is read");
        ck(json_get(j, "extract", v, sizeof(v)) && strcmp(v, "A microcontroller.") == 0,
           "and so is the one after it");
        ck(!json_get(j, "missing", v, sizeof(v)), "an absent key says so");

        // The escapes Wikipedia actually sends.
        const char *e = "{\"extract\":\"He said \\\"no\\\" and left.\\nThen \\\\ done\"}";
        ck(json_get(e, "extract", v, sizeof(v)), "an escaped field is read");
        ck(strcmp(v, "He said \"no\" and left.\nThen \\ done") == 0,
           "quotes, newlines and backslashes are unescaped");

        const char *u = "{\"extract\":\"caf\\u00e9 and \\u2014 dash\"}";
        ck(json_get(u, "extract", v, sizeof(v)), "a \\u field is read");
        ck(strcmp(v, "caf? and ? dash") == 0,
           "non-ASCII becomes '?' rather than a broken byte sequence");

        // A key that is a prefix of another must not match it.
        const char *p = "{\"titlecase\":\"wrong\",\"title\":\"right\"}";
        ck(json_get(p, "title", v, sizeof(v)) && strcmp(v, "right") == 0,
           "a longer key is not mistaken for a shorter one");

        // A truncated reply must fail rather than run off the end.
        const char *t = "{\"extract\":\"unterminated";
        ck(!json_get(t, "extract", v, sizeof(v)),
           "an unterminated string is refused");

        // A non-string value is not a string.
        const char *num = "{\"extract\":42}";
        ck(!json_get(num, "extract", v, sizeof(v)), "a number is not read as text");

        // A value longer than the buffer is clipped and still terminated.
        char small[8];
        const char *big = "{\"extract\":\"abcdefghijklmnop\"}";
        ck(json_get(big, "extract", small, sizeof(small)), "an overlong value returns");
        ck(strlen(small) < sizeof(small), "and is terminated inside the buffer");
    }

    // --- Wikipedia OpenSearch ------------------------------------------------
    {
        // The real shape: query, titles, descriptions, urls.
        const char *os_reply =
            "[\"pico\",[\"Raspberry Pi Pico\",\"Pico de Orizaba\",\"Picometre\"],"
            "[\"\",\"\",\"\"],"
            "[\"https://en.wikipedia.org/wiki/Raspberry_Pi_Pico\",\"\",\"\"]]";
        static char titles[4][FIELD_MAX];
        int n = wiki_titles(os_reply, titles, 4);
        ck(n == 3, "all three titles are found");
        ck(strcmp(titles[0], "Raspberry Pi Pico") == 0, "the first is right");
        ck(strcmp(titles[2], "Picometre") == 0, "and so is the last");

        // The query string is skipped, not returned as a title. This is the one
        // that silently puts the search term at the top of its own results.
        ck(strcmp(titles[0], "pico") != 0, "the query is not counted as a result");

        // No matches: an empty titles array.
        const char *empty = "[\"zzzz\",[],[],[]]";
        ck(wiki_titles(empty, titles, 4) == 0, "an empty result reads as none");

        // More results than there is room for: take what fits, do not overrun.
        const char *many =
            "[\"a\",[\"t1\",\"t2\",\"t3\",\"t4\",\"t5\",\"t6\"],[],[]]";
        ck(wiki_titles(many, titles, 4) == 4, "more results than slots fills the slots");

        // A query containing a bracket must not be mistaken for the array.
        const char *tricky = "[\"a[b\",[\"real\"],[],[]]";
        ck(wiki_titles(tricky, titles, 4) == 1 && strcmp(titles[0], "real") == 0,
           "a bracket inside the query does not shift the parse");
    }

    // --- wrapping -------------------------------------------------------------
    {
        reset_out();
        print_wrapped("one two three", "  ");
        ck(strcmp(g_out, "  one two three\n") == 0, "a short line is left alone");

        reset_out();
        // Eighty characters of five-letter words: it has to break, and only at
        // a space.
        print_wrapped("aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk "
                      "llll mmmm nnnn oooo pppp", "  ");
        bool broke = strchr(g_out, '\n') != strrchr(g_out, '\n');
        ck(broke, "a long line wraps");
        // No line may exceed the indent plus the column budget.
        {
            bool ok = true;
            const char *p = g_out;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len > 2 + WRAP_COLS) ok = false;
                if (!nl) break;
                p = nl + 1;
            }
            ck(ok, "and no line runs past the wrap column");
        }
        ck(strstr(g_out, "  aaaa") == g_out, "the indent is applied");

        reset_out();
        print_wrapped("first\nsecond", "  ");
        ck(strcmp(g_out, "  first\n  second\n") == 0,
           "an embedded newline indents the next line too");
    }

    printf("%s  %d checks", g_fails ? "FAILED" : "ok", g_checks);
    if (g_fails) printf(", %d failed", g_fails);
    printf("\n");
    return g_fails ? 1 : 0;
}
