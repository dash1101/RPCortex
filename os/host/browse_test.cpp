// browse: the page model and the URL rules, on the host.
//
// The fixtures are small pages written to have the same STRUCTURE as the sites
// the browser is meant for (a results table like DuckDuckGo's text version, a
// list of stories like Hacker News, a search form like Google's front page),
// not copies of them.
#include "../apps/browse/browse_page.cpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int g_fail, g_checks;
#define CK(cond, what) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static Page g_page;

// The page's text, in memory here (the package keeps it in a file).
static char     g_store[PAGE_TEXT + 16];
static uint32_t g_store_len;
static bool store_w(void *, uint32_t off, const char *d, uint32_t n) {
    if (off + n > sizeof(g_store)) return false;
    if (off == 0) g_store_len = 0;
    if (off != g_store_len) return false;           // appends only, in order
    memcpy(g_store + off, d, n); g_store_len = off + n;
    return true;
}
static uint32_t store_r(void *, uint32_t off, char *b, uint32_t n) {
    if (off >= g_store_len) return 0;
    if (off + n > g_store_len) n = g_store_len - off;
    memcpy(b, g_store + off, n);
    return n;
}

static void load(const char *html, int width, uint32_t piece = 0) {
    page_store(store_w, store_r, nullptr);
    page_begin(&g_page, width);
    uint32_t n = (uint32_t)strlen(html);
    if (!piece) page_feed(&g_page, html, n);
    else for (uint32_t i = 0; i < n; i += piece) page_feed(&g_page, html + i, n - i < piece ? n - i : piece);
    page_end(&g_page);
}

static std::string all_text() {
    std::string s;
    for (uint32_t i = 0; i < g_page.nlines; i++) {
        uint32_t len; const char *t = page_line(&g_page, i, &len);
        s.append(t, len); s += '\n';
    }
    return s;
}

static bool has(const char *needle) { return all_text().find(needle) != std::string::npos; }

static int link_named(const char *text) {
    // The link whose span covers `text` on some line.
    for (uint32_t i = 0; i < g_page.nspans; i++) {
        const Span &s = g_page.spans[i];
        if (s.kind != SPAN_LINK) continue;
        uint32_t len; const char *t = page_line(&g_page, s.line, &len);
        // columns == bytes for the ASCII fixtures
        std::string seen(t + s.col, s.width);
        if (seen.find(text) != std::string::npos) return s.ref;
    }
    return -1;
}

static int field_named(const char *name) {
    for (uint32_t i = 0; i < g_page.nfields; i++)
        if (!strcmp(page_str(&g_page, g_page.fields[i].name), name)) return (int)i;
    return -1;
}

static void check_lines_fit(int width, const char *what) {
    bool ok = true;
    for (uint32_t i = 0; i < g_page.nlines; i++) {
        uint32_t len; const char *t = page_line(&g_page, i, &len);
        if (text_columns(t, len) > (uint32_t)width) { ok = false; printf("    line %u is %u wide: %.*s\n", i, text_columns(t, len), (int)len, t); }
    }
    CK(ok, what);
}

static const char *kResults =
    "<!DOCTYPE html><html><head><title>pico w at DuckDuckGo</title>"
    "<style>body{color:red} a > b {}</style><script>var x = '<a href=\"/no\">no</a>';</script></head>"
    "<body><form action=\"/lite/\" method=\"post\">"
    "<input class=\"query\" type=\"text\" size=\"40\" name=\"q\" value=\"pico w\">"
    "<input type=\"hidden\" name=\"kl\" value=\"\">"
    "<input class=\"submit\" type=\"submit\" value=\"Search\"></form>"
    "<table border=\"0\">"
    "<tr><td>1.&nbsp;</td><td><a rel=\"nofollow\" href=\"https://www.raspberrypi.com/products/raspberry-pi-pico/\" class='result-link'>Raspberry Pi Pico W &amp; Pico WH</a></td></tr>"
    "<tr><td>&nbsp;&nbsp;&nbsp;</td><td class='result-snippet'>The <b>Pico W</b> adds on-board 2.4GHz wireless interfaces using an Infineon CYW43439.</td></tr>"
    "<tr><td>2.&nbsp;</td><td><a href=\"//example.org/pico?x=1&amp;y=2#top\" class='result-link'>Second result</a></td></tr>"
    "</table><!-- a comment with <a href=\"/hidden\">a link</a> --> <p>&copy; 2026 &#x41;&#66;C</p></body></html>";

static const char *kStories =
    "<html><head><title>Stories</title></head><body><center><table>"
    "<tr class='athing'><td class='title'><span class='rank'>1.</span></td>"
    "<td class='title'><span class='titleline'><a href=\"https://example.com/a-very-long-story-title\">A very long story title that will have to wrap across more than one line of a narrow screen</a> (<a href=\"from?site=example.com\"><span class='sitestr'>example.com</span></a>)</span></td></tr>"
    "<tr><td class='subtext'>120 points by <a href=\"user?id=someone\">someone</a> | <a href=\"item?id=1\">45&nbsp;comments</a></td></tr>"
    "</table></center><ul><li>one</li><li>two<ul><li>inner</li></ul></li></ul><pre>  a\tb\n  c</pre></body></html>";

static const char *kSearchHome =
    "<html><head><title>Google</title></head><body>"
    "<form action=\"/search\" name=\"f\"><input type=hidden name=ie value=\"ISO-8859-1\">"
    "<input autocomplete=\"off\" class=lst value=\"\" title=\"Google Search\" maxlength=\"2048\" name=\"q\" size=\"57\">"
    "<input class=lsb value=\"Google Search\" name=\"btnG\" type=\"submit\">"
    "<input class=lsb value=\"I'm Feeling Lucky\" name=\"btnI\" type=\"submit\">"
    "<select name=\"lang\"><option value=\"en\">English<option value=\"de\" selected>Deutsch</select>"
    "<input type=checkbox name=safe value=active checked><input type=checkbox name=other>"
    "<textarea name=notes>first\nsecond</textarea>"
    "<button type=submit name=go value=1>Go <b>now</b></button>"
    "</form></body></html>";

int main() {
    // --- a results page ---------------------------------------------------
    load(kResults, 80);
    CK(!strcmp(g_page.title, "pico w at DuckDuckGo"), "title");
    CK(has("Raspberry Pi Pico W & Pico WH"), "link text with an entity");
    CK(has("adds on-board 2.4GHz"), "snippet text");
    CK(!has("var x"), "script dropped");
    CK(!has("color:red"), "style dropped");
    CK(!has("a comment"), "comment dropped");
    CK(has("(c) 2026 ABC"), "copyright and numeric entities, folded to ASCII");
    int r1 = link_named("Raspberry Pi Pico W");
    CK(r1 >= 0 && !strcmp(page_str(&g_page, g_page.links[r1]), "https://www.raspberrypi.com/products/raspberry-pi-pico/"),
       "first result's target");
    int r2 = link_named("Second result");
    CK(r2 >= 0, "second result is a link");
    char url[URL_MAX];
    CK(r2 >= 0 && url_resolve("https://lite.duckduckgo.com/lite/", page_str(&g_page, g_page.links[r2]), url, sizeof(url))
       && !strcmp(url, "https://example.org/pico?x=1&y=2"), "protocol-relative link, entity in it, fragment dropped");
    CK(g_page.nforms == 1 && g_page.forms[0].post, "the form, a POST");
    int q = field_named("q");
    CK(q >= 0 && !strcmp(g_page.fields[q].value, "pico w"), "the query box keeps its value");
    CK(field_named("kl") >= 0 && g_page.fields[field_named("kl")].type == FIELD_HIDDEN, "hidden field");
    {
        char body[512], note[128];
        int sub = -1;
        for (uint32_t i = 0; i < g_page.nfields; i++) if (g_page.fields[i].type == FIELD_SUBMIT) sub = (int)i;
        int how = form_submit(&g_page, "https://lite.duckduckgo.com/lite/?q=x", 0, sub, url, sizeof(url), body, sizeof(body), note, sizeof(note));
        CK(how == 2, "POST");
        CK(!strcmp(url, "https://lite.duckduckgo.com/lite/"), "POST goes to the action");
        CK(!strcmp(body, "q=pico+w&kl="), "POST body: the fields, the unnamed button left out");
    }
    check_lines_fit(80, "every line fits 80 columns");

    // The same page fed a byte at a time, and in odd pieces, lays out the same.
    std::string whole = all_text();
    load(kResults, 80, 1);
    CK(all_text() == whole, "fed a byte at a time: same layout");
    load(kResults, 80, 7);
    CK(all_text() == whole, "fed 7 bytes at a time: same layout");

    // --- stories, narrow ------------------------------------------------------
    load(kStories, 30);
    check_lines_fit(30, "every line fits 30 columns");
    int st = link_named("A very long");
    int st2 = link_named("screen");
    CK(st >= 0 && st == st2, "a link that wraps is one link across its lines");
    uint32_t pieces = 0;
    for (uint32_t i = 0; i < g_page.nspans; i++) if (g_page.spans[i].kind == SPAN_LINK && g_page.spans[i].ref == (uint16_t)st) pieces++;
    CK(pieces >= 3, "and has a span on each line");
    int cm = link_named("45");
    CK(cm >= 0 && url_resolve("https://news.ycombinator.com/", page_str(&g_page, g_page.links[cm]), url, sizeof(url))
       && !strcmp(url, "https://news.ycombinator.com/item?id=1"), "relative link against the site root");
    CK(has("* one") && has("* two") && has("  * inner"), "lists, nested one indented");
    CK(has("  a     b") && has("  c"), "pre keeps spaces, a tab to the next 8th column");

    // --- a search box --------------------------------------------------------
    load(kSearchHome, 100);
    int gq = field_named("q");
    CK(gq >= 0 && g_page.fields[gq].type == FIELD_TEXT, "the search box");
    int lang = field_named("lang");
    CK(lang >= 0 && !strcmp(g_page.fields[lang].value, "de"), "drop-down: the selected option's value");
    CK(lang >= 0 && !strcmp(g_page.fields[lang].value + 3, "Deutsch"), "and its text");
    int notes = field_named("notes");
    CK(notes >= 0 && !strcmp(g_page.fields[notes].value, "first second"), "textarea content");
    int go = field_named("go");
    CK(go >= 0 && g_page.fields[go].type == FIELD_SUBMIT && !strcmp(g_page.fields[go].value, "1"), "button: its value is sent");
    CK(!has("now"), "button label is not page text");
    {
        Page &pg = g_page;
        copy(pg.fields[gq].value, sizeof(pg.fields[gq].value), "raspberry pi");
        char body[256], note[128];
        int btn = field_named("btnG");
        int how = form_submit(&pg, "https://www.google.com/", 0, btn, url, sizeof(url), body, sizeof(body), note, sizeof(note));
        CK(how == 1 && !strcmp(url, "https://lite.duckduckgo.com/lite/?q=raspberry+pi"), "Google's search goes to DuckDuckGo");
        CK(note[0] != 0, "and says so");
        how = form_submit(&pg, "https://example.com/", 0, btn, url, sizeof(url), body, sizeof(body), note, sizeof(note));
        CK(how == 1 && !strcmp(url, "https://example.com/search?ie=ISO-8859-1&q=raspberry+pi&btnG=Google+Search&lang=de&safe=active&notes=first+second"),
           "elsewhere: GET with every field, checked boxes only, the pressed button only");
        CK(note[0] == 0, "and no note");
    }

    // --- URLs ------------------------------------------------------------------
    struct { const char *base, *href, *want; } R[] = {
        {"https://a.com/x/y/z.html", "w.html", "https://a.com/x/y/w.html"},
        {"https://a.com/x/y/z.html", "../w.html", "https://a.com/x/w.html"},
        {"https://a.com/x/y/z.html", "../../../w.html", "https://a.com/w.html"},
        {"https://a.com/x/y/", "./", "https://a.com/x/y/"},
        {"https://a.com/x/y/z.html", "/abs?q=1", "https://a.com/abs?q=1"},
        {"https://a.com/x/y/z.html?old=1", "?new=2", "https://a.com/x/y/z.html?new=2"},
        {"https://a.com", "page", "https://a.com/page"},
        {"https://a.com/x", "#frag", "https://a.com/x"},
        {"http://a.com:8080/x/", "y", "http://a.com:8080/x/y"},
        {"https://a.com/", "HTTPS://B.com/Path", "HTTPS://B.com/Path"},
        {"https://a.com/", "  /spaced  ", "https://a.com/spaced"},
    };
    for (auto &c : R) {
        bool ok = url_resolve(c.base, c.href, url, sizeof(url)) && !strcmp(url, c.want);
        if (!ok) printf("    %s + %s -> %s (want %s)\n", c.base, c.href, url, c.want);
        CK(ok, "url_resolve");
    }
    CK(!url_resolve("https://a.com/", "mailto:x@y.z", url, sizeof(url)), "mailto: is not followed");
    CK(!url_resolve("https://a.com/", "javascript:void(0)", url, sizeof(url)), "javascript: is not followed");

    struct { const char *in, *want; } U[] = {
        {"google", "https://www.google.com/"},
        {"news.ycombinator.com", "https://news.ycombinator.com/"},
        {"example.com/a?b=c", "https://example.com/a?b=c"},
        {"http://plain.example/", "http://plain.example/"},
        {"raspberry pi pico", "https://lite.duckduckgo.com/lite/?q=raspberry+pi+pico"},
        {"  text.npr.org  ", "https://text.npr.org/"},
        {"c++", "https://lite.duckduckgo.com/lite/?q=c%2B%2B"},
    };
    for (auto &c : U) {
        url_from_input(c.in, url, sizeof(url));
        if (strcmp(url, c.want)) printf("    '%s' -> %s (want %s)\n", c.in, url, c.want);
        CK(!strcmp(url, c.want), "url_from_input");
    }

    // --- limits ----------------------------------------------------------------
    {
        // Far more text than the page holds: it stops cleanly and says so.
        std::string big = "<p>";
        for (int i = 0; i < 9000; i++) big += "word" + std::to_string(i) + " ";
        load(big.c_str(), 80);
        CK(g_page.truncated, "an oversized page is marked truncated");
        CK(g_page.text_len <= PAGE_TEXT && g_store_len == g_page.text_len, "and stays inside its store, all of it written");
        check_lines_fit(80, "lines still fit");
        // Malformed: an unclosed tag, a stray '<', an unterminated entity.
        load("a < b and c <3 d &amp e &notreal; <b>bold <i>x", 40);
        CK(has("a < b and c <3 d &amp e &notreal; bold x"), "malformed input reads as text");
        // A very long word is cut at the width.
        std::string w(250, 'x');
        load(w.c_str(), 40);
        check_lines_fit(40, "a 250-letter word is cut at the width");
        CK(g_page.nlines == 7, "into 7 lines");
        // UTF-8 is folded to ASCII: one byte a column, whole characters only.
        load("caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 5\xE2\x82\xAC \xE4\xB8\xAD &eacute;&#8217;", 60);
        CK(has("cafe \"quoted\" - 5EUR ? e'"), "UTF-8 and entities folded to ASCII");
        bool ascii = true;
        for (uint32_t i = 0; i < g_store_len; i++) if ((unsigned char)g_store[i] >= 0x80) ascii = false;
        CK(ascii, "the page holds ASCII only");
        load("\xC3 broken \xA9 \xE2\x82", 60);
        CK(has("? broken ? ?"), "broken UTF-8 shows as '?'");
    }

    // --- hostile input --------------------------------------------------------
    // The fixtures with bytes flipped, inserted and cut, fed in random pieces.
    // Nothing to check but that it stays inside its buffers (the sanitizers say)
    // and every line fits.
    {
        uint32_t seed = 12345;
        auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) & 0xFFFFFF; };
        const char *src[] = {kResults, kStories, kSearchHome};
        const char spicy[] = "<>&;\"'=/!-#x \n\t\xC3\xA9\xE2";
        bool fit = true;
        for (int it = 0; it < 4000; it++) {
            std::string h = src[it % 3];
            int edits = 1 + (int)(rnd() % 40);
            for (int e = 0; e < edits && !h.empty(); e++) {
                size_t at = rnd() % h.size();
                switch (rnd() % 3) {
                case 0: h[at] = spicy[rnd() % (sizeof(spicy) - 1)]; break;
                case 1: h.insert(at, 1, spicy[rnd() % (sizeof(spicy) - 1)]); break;
                case 2: h.erase(at, 1 + rnd() % 20); break;
                }
            }
            int width = 20 + (int)(rnd() % 120);
            page_store(store_w, store_r, nullptr);
            page_begin(&g_page, width);
            size_t i = 0;
            while (i < h.size()) { size_t n = 1 + rnd() % 64; if (i + n > h.size()) n = h.size() - i; page_feed(&g_page, h.data() + i, (uint32_t)n); i += n; }
            page_end(&g_page);
            for (uint32_t l = 0; l < g_page.nlines; l++) {
                uint32_t len; const char *t = page_line(&g_page, l, &len);
                if (text_columns(t, len) > (uint32_t)width) fit = false;
            }
            for (uint32_t k = 0; k < g_page.nspans; k++) if (g_page.spans[k].line > g_page.nlines) fit = false;
            char body[512], note[128];
            for (uint32_t f = 0; f < g_page.nforms; f++)
                form_submit(&g_page, "https://a.example/x/", (int)f, -1, url, sizeof(url), body, sizeof(body), note, sizeof(note));
            for (uint32_t k = 0; k < g_page.nlinks; k++)
                url_resolve("https://a.example/x/y?z", page_str(&g_page, g_page.links[k]), url, sizeof(url));
        }
        CK(fit, "4000 damaged pages: every line fits and every span is on a line");
        // URLs: random strings against random bases.
        for (int it = 0; it < 4000; it++) {
            char a[80], b[80];
            for (int k = 0; k < 79; k++) { a[k] = spicy[rnd() % (sizeof(spicy) - 1)]; b[k] = spicy[rnd() % (sizeof(spicy) - 1)]; }
            a[rnd() % 79] = 0; b[rnd() % 79] = 0;
            char base[100]; snprintf(base, sizeof(base), "https://h.example/%s", a);
            url_resolve(base, b, url, sizeof(url));
            url_from_input(b, url, sizeof(url));
        }
        CK(true, "4000 random URLs did not fault");
    }

    printf("browse_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
