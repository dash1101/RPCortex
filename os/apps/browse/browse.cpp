// browse — the web, as text, on the console.
//
//   browse [url | words]
//   browse -d <url | words> [-w width]   print the page and its links, and exit
//
// Type an address (or a site's name: "google" opens www.google.com, words with
// spaces are searched), move between the links and the boxes on the page with
// the arrow keys, type into a box, press Enter. Left goes back.
//
// It shows what a page's HTML says: text, links, headings, lists and forms. It
// runs no JavaScript and loads no images or styles, so a page that builds itself
// in a script shows little. Google refuses every browser like that for its
// search results; its search box sends the words to DuckDuckGo's text version
// instead, and says so.
//
// Pages arrive in a file (/tmp/browse.html), at most BROWSE_CAP bytes of each,
// and are laid out from it a kilobyte at a time. A board without Wi-Fi gets the
// web through RPC Studio on the computer it is plugged into.
#include "rpc_app.h"
#include "browse_page.h"

#include <stdio.h>
#include <string.h>

RPC_APP_VER("browse", "1.0");

#define BROWSE_FILE  "/tmp/browse.html"
#define BROWSE_CAP   (96u * 1024u)     // of any one page; the rest is not fetched
#define HISTORY_N    8
#define BROWSE_UA    "RPCortex-browse/1.0 (text-mode; Lynx-compatible)"

#define C_LINK   6        // cyan
#define C_FIELD  3        // yellow
#define C_HEAD   6
#define C_WARN   3

// Append `s` to `dst` (a buffer of `cap`), keeping it terminated.
static void append(char *dst, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(dst);
    while (*s && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
}

static Page g_pg;                         // the page on screen (~45 KB, not on any stack)
static FwHttpRequest g_req;               // a request and its answer

struct Hist { char url[URL_MAX]; int top, focus; };
static Hist g_hist[HISTORY_N];
static int  g_hist_n;

static char g_url[URL_MAX];               // the page on screen
static char g_status[160];                // the bottom line, once
static char g_note[120];                  // a line above the page, while it is shown
static int  g_w, g_h;                     // the terminal
static int  g_top;                        // first page line shown
static int  g_focus = -1;                 // span index of the focused link or field
static bool g_tui;                        // full screen (false: browse -d, printing)

// --- focus -----------------------------------------------------------------------
//
// What the arrows move between: a link (its first span; a link that wraps has
// several) or a visible field.

static bool focusable(int i) {
    const Span &s = g_pg.spans[i];
    if (s.kind == SPAN_FIELD) return g_pg.fields[s.ref].type != FIELD_HIDDEN;
    if (s.kind != SPAN_LINK) return false;
    for (int j = i - 1; j >= 0; j--) {             // not a continuation of the same link
        const Span &t = g_pg.spans[j];
        if (t.kind == SPAN_LINK) return t.ref != s.ref;
    }
    return true;
}

static int next_focus(int from, int dir) {
    for (int i = from + dir; i >= 0 && i < (int)g_pg.nspans; i += dir)
        if (focusable(i)) return i;
    return -1;
}

static int view_rows() { return g_h - 2 - (g_note[0] ? 1 : 0); }
static int view_y0()   { return 1 + (g_note[0] ? 1 : 0); }

static void scroll_to(int line) {
    int rows = view_rows();
    if (line < g_top) g_top = line;
    else if (line >= g_top + rows) g_top = line - rows + 1;
    int max_top = (int)g_pg.nlines - rows;
    if (g_top > max_top) g_top = max_top;
    if (g_top < 0) g_top = 0;
}

static bool visible(int span) {
    int l = g_pg.spans[span].line;
    return l >= g_top && l < g_top + view_rows();
}

static void focus_first_visible() {
    g_focus = -1;
    for (int i = 0; i < (int)g_pg.nspans; i++)
        if (focusable(i) && visible(i)) { g_focus = i; return; }
}

static bool focus_is_text() {
    if (g_focus < 0) return false;
    const Span &s = g_pg.spans[g_focus];
    if (s.kind != SPAN_FIELD) return false;
    uint8_t t = g_pg.fields[s.ref].type;
    return t == FIELD_TEXT || t == FIELD_PASSWORD || t == FIELD_TEXTAREA;
}

// --- drawing ---------------------------------------------------------------------

static void field_text(const Field &f, int width, char *out, uint32_t cap) {
    int inner = width - 2;
    if (inner < 1) inner = 1;
    switch (f.type) {
    case FIELD_CHECKBOX: snprintf(out, cap, "[%c]", f.checked ? 'x' : ' '); return;
    case FIELD_RADIO:    snprintf(out, cap, "(%c)", f.checked ? '*' : ' '); return;
    case FIELD_SUBMIT:   snprintf(out, cap, "[ %s ]", f.value[0] ? f.value : "Submit"); return;
    case FIELD_SELECT: {
        const char *shown = f.value + strlen(f.value) + 1;
        snprintf(out, cap, "[%.*s v]", inner - 2 > 0 ? inner - 2 : 1, shown);
        return;
    }
    default: break;
    }
    // A text box: the end of what was typed, then underscores to its width.
    char body[PAGE_WIDTH_MAX + 1];
    int n = (int)strlen(f.value);
    int start = n > inner ? n - inner : 0;
    int k = 0;
    for (int i = start; i < n && k < inner; i++) body[k++] = f.type == FIELD_PASSWORD ? '*' : f.value[i];
    while (k < inner) body[k++] = '_';
    body[k] = 0;
    snprintf(out, cap, "[%s]", body);
}

static void draw() {
    fw_tui_clear();
    char bar[PAGE_WIDTH_MAX + 1];
    // The top line: what this is.
    snprintf(bar, sizeof(bar), " %s ", g_pg.title[0] ? g_pg.title : g_url);
    fw_tui_fill(0, 0, g_w, 1, ' ', FW_ATTR_REVERSE, 0);
    fw_tui_text(0, 0, bar, FW_ATTR_REVERSE | FW_ATTR_BOLD, 0);
    if (g_note[0]) fw_tui_text(1, 1, g_note, FW_ATTR_BOLD, C_WARN);

    int y0 = view_y0(), rows = view_rows();
    for (int r = 0; r < rows; r++) {
        uint32_t li = (uint32_t)(g_top + r);
        if (li >= g_pg.nlines) break;
        uint32_t len;
        const char *t = page_line(&g_pg, li, &len);
        if ((int)len > g_w) len = (uint32_t)g_w;
        memcpy(bar, t, len); bar[len] = 0;
        fw_tui_text(0, y0 + r, bar, FW_ATTR_NORMAL, 0);
    }
    // Links, bold and fields over the text.
    for (uint32_t i = 0; i < g_pg.nspans; i++) {
        const Span &s = g_pg.spans[i];
        int r = (int)s.line - g_top;
        if (r < 0 || r >= rows || s.col >= g_w) continue;
        int w = s.width;
        if (s.col + w > g_w) w = g_w - s.col;
        bool focused = false;
        if (g_focus >= 0) {
            const Span &f = g_pg.spans[g_focus];
            focused = (f.kind == s.kind && f.ref == s.ref && (s.kind == SPAN_LINK || (int)i == g_focus));
        }
        if (s.kind == SPAN_FIELD) {
            char ft[PAGE_WIDTH_MAX + 1];
            field_text(g_pg.fields[s.ref], s.width, ft, sizeof(ft));
            ft[w < (int)sizeof(ft) ? w : (int)sizeof(ft) - 1] = 0;
            fw_tui_text(s.col, y0 + r, ft, focused ? FW_ATTR_REVERSE : FW_ATTR_BOLD, C_FIELD);
            continue;
        }
        uint32_t len;
        const char *t = page_line(&g_pg, s.line, &len);
        if (s.col + w > (int)len) w = (int)len - s.col;
        if (w <= 0) continue;
        memcpy(bar, t + s.col, (size_t)w); bar[w] = 0;
        if (s.kind == SPAN_LINK)
            fw_tui_text(s.col, y0 + r, bar, focused ? FW_ATTR_REVERSE : FW_ATTR_UNDER, C_LINK);
        else
            fw_tui_text(s.col, y0 + r, bar, FW_ATTR_BOLD, s.kind == SPAN_HEAD ? C_HEAD : 0);
    }
    // The bottom line: a message, or where the focused link goes, or the keys.
    const char *foot = g_status;
    char where[URL_MAX + 8];
    if (!foot[0] && g_focus >= 0 && g_pg.spans[g_focus].kind == SPAN_LINK) {
        snprintf(where, sizeof(where), "-> %s", page_str(&g_pg, g_pg.links[g_pg.spans[g_focus].ref]));
        foot = where;
    }
    if (!foot[0])
        foot = focus_is_text() ? "type, Enter sends  Up/Down move  Ctrl+L address  Ctrl+C quit"
                               : "Up/Down move  Enter open  Left back  g address  r reload  q quit";
    snprintf(bar, sizeof(bar), "%.*s", g_w - 1, foot);
    fw_tui_text(0, g_h - 1, bar, FW_ATTR_DIM, 0);
    if (g_pg.nlines > (uint32_t)rows) {
        char pos[24];
        uint32_t pct = (uint32_t)(g_top + rows) * 100u / g_pg.nlines;
        snprintf(pos, sizeof(pos), " %u%% ", pct > 100 ? 100 : pct);
        fw_tui_text(g_w - (int)strlen(pos), g_h - 1, pos, FW_ATTR_REVERSE, 0);
    }
    fw_tui_present();
}

static void status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s);
}

// --- loading ---------------------------------------------------------------------

static void error_page(const char *url, const char *why) {
    page_begin(&g_pg, g_w);
    char html[700];
    snprintf(html, sizeof(html),
             "<title>Could not load the page</title><h1>Could not load the page</h1>"
             "<p>%s</p><p>%s</p><p>Check the address, or press g to type another. "
             "A board without Wi-Fi goes online through RPC Studio on the computer it is "
             "plugged into (rpc term, or rpc bridge).</p>", url, why);
    page_feed(&g_pg, html, (uint32_t)strlen(html));
    page_end(&g_pg);
}

// Lay the file out. HTML unless the server said otherwise.
static void layout(const char *type) {
    page_begin(&g_pg, g_w);
    bool plain = !strncmp(type, "text/plain", 10);
    bool html = !type[0] || strstr(type, "html") || strstr(type, "xml");
    if (!plain && !html && strncmp(type, "text/", 5) && !strstr(type, "json")) {
        char msg[200];
        snprintf(msg, sizeof(msg), "<title>Not a page</title><p>This is %s (%u bytes). browse shows text and HTML only.</p>",
                 type, (unsigned)fw_file_size(BROWSE_FILE));
        page_feed(&g_pg, msg, (uint32_t)strlen(msg));
        page_end(&g_pg);
        return;
    }
    static char chunk[1024];
    uint32_t off = 0;
    for (;;) {
        uint32_t n = fw_file_read_at(BROWSE_FILE, off, chunk, sizeof(chunk));
        if (!n) break;
        if (html && !plain) page_feed(&g_pg, chunk, n);
        else page_feed_text(&g_pg, chunk, n);
        off += n;
        if (n < sizeof(chunk)) break;
    }
    page_end(&g_pg);
}

// Fetch `url` (POST when `body` is given) and show it. False when nothing
// arrived; the error is then the page.
static bool load(const char *url0, const char *body) {
    char url[URL_MAX];
    snprintf(url, sizeof(url), "%s", url0);
    snprintf(g_status, sizeof(g_status), "Loading %s ...", url);
    if (g_tui) draw();
    for (int hop = 0; hop < 4; hop++) {
        memset(&g_req, 0, sizeof(g_req));
        g_req.size = sizeof(g_req);
        g_req.url = url;
        g_req.method = body ? "POST" : nullptr;
        g_req.body = body;
        g_req.body_len = body ? (unsigned)strlen(body) : 0;
        g_req.user_agent = BROWSE_UA;
        g_req.path = BROWSE_FILE;
        g_req.max_bytes = BROWSE_CAP;
        int n = fw_http_request(&g_req);
        if (n < 0) {
            error_page(url, g_req.detail[0] ? g_req.detail : "The request failed.");
            snprintf(g_url, sizeof(g_url), "%s", url);
            g_status[0] = 0;
            return false;
        }
        // A redirect that was not followed for us (a POST's 303): GET it.
        if (g_req.status >= 300 && g_req.status < 400 && g_req.location[0]) {
            char next[URL_MAX];
            const char *base = g_req.final_url[0] ? g_req.final_url : url;
            if (!url_resolve(base, g_req.location, next, sizeof(next))) break;
            snprintf(url, sizeof(url), "%s", next);
            body = nullptr;
            continue;
        }
        break;
    }
    snprintf(g_url, sizeof(g_url), "%s", g_req.final_url[0] ? g_req.final_url : url);
    layout(g_req.content_type_out);
    g_top = 0;
    focus_first_visible();
    // The first text box, on a page that has one near the top: where a search
    // page wants the cursor.
    for (int i = 0; i < (int)g_pg.nspans; i++) {
        const Span &s = g_pg.spans[i];
        if (!visible(i)) break;
        if (s.kind == SPAN_FIELD && g_pg.fields[s.ref].type == FIELD_TEXT) { g_focus = i; break; }
    }
    g_status[0] = 0;
    if (g_req.status != 200) snprintf(g_status, sizeof(g_status), "HTTP %d", g_req.status);
    if (g_req.truncated)
        snprintf(g_status, sizeof(g_status), "Only the first %u KB of this page was loaded.", BROWSE_CAP / 1024);
    else if (g_pg.truncated)
        status("This page is longer than browse can hold; the end is missing.");
    return true;
}

static void remember() {
    if (!g_url[0]) return;
    if (g_hist_n == HISTORY_N) {
        memmove(g_hist, g_hist + 1, sizeof(Hist) * (HISTORY_N - 1));
        g_hist_n--;
    }
    Hist &h = g_hist[g_hist_n++];
    snprintf(h.url, sizeof(h.url), "%s", g_url);
    h.top = g_top; h.focus = g_focus;
}

static void go(const char *url, const char *body) {
    remember();
    g_note[0] = 0;
    load(url, body);
}

static void back() {
    if (!g_hist_n) { status("Nothing to go back to."); return; }
    Hist h = g_hist[--g_hist_n];
    g_note[0] = 0;
    if (load(h.url, nullptr)) {
        g_top = h.top;
        g_focus = (h.focus >= 0 && h.focus < (int)g_pg.nspans) ? h.focus : -1;
        scroll_to(g_top);
    }
}

// --- the address bar ---------------------------------------------------------------

static bool ask_address(char *out, uint32_t cap) {
    char typed[URL_MAX] = "";
    uint32_t n = 0;
    for (;;) {
        char line[PAGE_WIDTH_MAX + 16];
        int room = g_w - 7;
        const char *shown = n > (uint32_t)room ? typed + n - room : typed;
        snprintf(line, sizeof(line), " Go: %s_", shown);
        fw_tui_fill(0, g_h - 1, g_w, 1, ' ', FW_ATTR_REVERSE, 0);
        fw_tui_text(0, g_h - 1, line, FW_ATTR_REVERSE, 0);
        fw_tui_present();
        FwTuiEvent e;
        for (;;) {
            if (fw_tui_poll(&e)) break;
            if (fw_task_should_stop()) return false;
            fw_task_sleep_ms(5);
        }
        if (e.kind != 1) continue;
        if (e.key == 13 || e.key == 10) break;
        if (e.key == FW_KEY_ESC || e.key == 3) return false;
        if (e.key == 127 || e.key == 8) { if (n) typed[--n] = 0; continue; }
        if (e.key == 21) { n = 0; typed[0] = 0; continue; }          // Ctrl+U
        if (e.key >= 32 && e.key < 127 && n + 1 < sizeof(typed)) { typed[n++] = (char)e.key; typed[n] = 0; }
    }
    if (!n) return false;
    url_from_input(typed, out, cap);
    return true;
}

// --- acting on the focus -------------------------------------------------------------

static void submit(int form, int pressed) {
    char url[URL_MAX], note[120];
    static char body[1024];
    int how = form_submit(&g_pg, g_url, form, pressed, url, sizeof(url), body, sizeof(body), note, sizeof(note));
    if (!how) { status("This form could not be sent (too long, or no address)."); return; }
    go(url, how == 2 ? body : nullptr);
    if (note[0]) snprintf(g_note, sizeof(g_note), "%s", note);
}

static int first_submit(int form) {
    for (uint32_t i = 0; i < g_pg.nfields; i++)
        if (g_pg.fields[i].form == form && g_pg.fields[i].type == FIELD_SUBMIT) return (int)i;
    return -1;
}

static void activate() {
    if (g_focus < 0) return;
    const Span &s = g_pg.spans[g_focus];
    if (s.kind == SPAN_LINK) {
        char url[URL_MAX];
        if (!url_resolve(g_url, page_str(&g_pg, g_pg.links[s.ref]), url, sizeof(url))) {
            status("That link is not a web page (mail, a script, another kind of address).");
            return;
        }
        go(url, nullptr);
        return;
    }
    Field &f = g_pg.fields[s.ref];
    switch (f.type) {
    case FIELD_CHECKBOX: f.checked = !f.checked; return;
    case FIELD_RADIO:
        for (uint32_t i = 0; i < g_pg.nfields; i++) {
            Field &o = g_pg.fields[i];
            if (o.type == FIELD_RADIO && o.form == f.form && o.name == f.name) o.checked = false;
        }
        f.checked = true;
        return;
    case FIELD_SELECT:
        status("Choosing another option is not supported yet; the one shown is sent.");
        return;
    case FIELD_SUBMIT:
        if (f.form == 0xFF) { status("This button belongs to no form."); return; }
        submit(f.form, (int)s.ref);
        return;
    default:
        // Enter in a text box sends its form, as the first button would.
        if (f.form == 0xFF) return;
        submit(f.form, first_submit(f.form));
        return;
    }
}

static void move(int dir) {
    int rows = view_rows();
    int n = next_focus(g_focus < 0 ? (dir > 0 ? -1 : (int)g_pg.nspans) : g_focus, dir);
    // The next link, if it is on screen or just past it; otherwise scroll a line.
    if (n >= 0) {
        int l = g_pg.spans[n].line;
        if (l >= g_top - 1 && l <= g_top + rows) { g_focus = n; scroll_to(l); return; }
    }
    int max_top = (int)g_pg.nlines - rows;
    if (dir > 0 && g_top < max_top) g_top++;
    else if (dir < 0 && g_top > 0) g_top--;
    else if (n >= 0) { g_focus = n; scroll_to(g_pg.spans[n].line); }
    if (g_focus >= 0 && !visible(g_focus)) focus_first_visible();
}

static void page(int dir) {
    int rows = view_rows();
    g_top += dir * (rows - 1);
    scroll_to(g_top);
    int max_top = (int)g_pg.nlines - rows;
    if (g_top > max_top) g_top = max_top > 0 ? max_top : 0;
    focus_first_visible();
}


// browse -d: the page as text, then its links numbered, the way `lynx -dump`
// prints one. For scripts, for a quick read, and for testing the whole path
// without a screen to drive.
static int dump(int argc, char **argv) {
    char typed[URL_MAX] = "";
    g_w = 80;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) {
            int v = 0;
            for (const char *c = argv[++i]; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
            if (v >= 20 && v <= PAGE_WIDTH_MAX) g_w = v;
            continue;
        }
        if (typed[0]) append(typed, sizeof(typed), " ");
        append(typed, sizeof(typed), argv[i]);
    }
    if (!typed[0]) { fw_printf("Usage: browse -d <url | words> [-w width]\n"); return 1; }
    g_tui = false;
    g_h = 24;
    char url[URL_MAX];
    url_from_input(typed, url, sizeof(url));
    bool ok = load(url, nullptr);
    if (g_pg.title[0]) fw_printf("%s\n", g_pg.title);
    fw_printf("%s\n\n", g_url);
    char line[PAGE_WIDTH_MAX + 1];
    for (uint32_t i = 0; i < g_pg.nlines; i++) {
        uint32_t len;
        const char *t = page_line(&g_pg, i, &len);
        memcpy(line, t, len); line[len] = 0;
        // Fields show as they would on screen.
        for (uint32_t k = 0; k < g_pg.nspans; k++) {
            const Span &sp = g_pg.spans[k];
            if (sp.line != i || sp.kind != SPAN_FIELD) continue;
            char ft[PAGE_WIDTH_MAX + 1];
            field_text(g_pg.fields[sp.ref], sp.width, ft, sizeof(ft));
            for (int c = 0; c < sp.width && ft[c] && sp.col + c < (int)len; c++) line[sp.col + c] = ft[c];
        }
        fw_printf("%s\n", line);
    }
    if (g_pg.nlinks) {
        fw_printf("\nLinks\n");
        for (uint32_t i = 0; i < g_pg.nlinks; i++) {
            char abs[URL_MAX];
            const char *h = page_str(&g_pg, g_pg.links[i]);
            fw_printf("%3u. %s\n", (unsigned)(i + 1), url_resolve(g_url, h, abs, sizeof(abs)) ? abs : h);
        }
    }
    if (g_status[0]) fw_printf("\n(%s)\n", g_status);
    fw_file_remove(BROWSE_FILE);
    return ok ? 0 : 1;
}

// --- the loop ----------------------------------------------------------------------

static int browse_cmd(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-d")) return dump(argc, argv);
    g_tui = true;
    fw_tui_begin();
    fw_tui_size(&g_w, &g_h);
    if (g_w > PAGE_WIDTH_MAX) g_w = PAGE_WIDTH_MAX;
    g_hist_n = 0; g_note[0] = 0; g_status[0] = 0; g_url[0] = 0; g_focus = -1; g_top = 0;

    char start[URL_MAX];
    if (argc > 1) {
        char typed[URL_MAX] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) append(typed, sizeof(typed), " ");
            append(typed, sizeof(typed), argv[i]);
        }
        url_from_input(typed, start, sizeof(start));
        load(start, nullptr);
    } else {
        page_begin(&g_pg, g_w);
        static const char kHome[] =
            "<title>browse</title><h1>browse</h1>"
            "<p>The web, as text. Press <b>g</b> and type an address, a site's name "
            "(google, wikipedia) or some words to search for.</p>"
            "<p>Up and Down move between the links and boxes, Enter opens or sends, "
            "Left goes back, q quits.</p>"
            "<ul><li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo</a> (search)</li>"
            "<li><a href=\"https://text.npr.org/\">NPR News</a></li>"
            "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
            "<li><a href=\"https://en.m.wikipedia.org/wiki/Special:Random\">A random Wikipedia article</a></li>"
            "<li><a href=\"http://frogfind.com/\">FrogFind</a> (search, made for old computers)</li></ul>"
            "<p>No scripts, styles or pictures. Pages built by JavaScript show little.</p>";
        page_feed(&g_pg, kHome, sizeof(kHome) - 1);
        page_end(&g_pg);
        snprintf(g_url, sizeof(g_url), "about:home");
        focus_first_visible();
    }

    for (;;) {
        draw();
        g_status[0] = 0;
        FwTuiEvent e;
        bool got = false;
        while (!got) {
            if (fw_tui_poll(&e)) { got = true; break; }
            if (fw_task_should_stop()) break;
            fw_task_sleep_ms(5);
        }
        if (!got) break;

        if (e.kind == 2) {                                 // the mouse
            if (e.mouse == 3) { g_top -= 3; scroll_to(g_top); }
            else if (e.mouse == 4) { g_top += 3; scroll_to(g_top + view_rows() - 1); }
            else if (e.mouse == 0) {
                int line = g_top + (int)e.y - view_y0();
                for (int i = 0; i < (int)g_pg.nspans; i++) {
                    const Span &s = g_pg.spans[i];
                    if (s.line == line && e.x >= s.col && e.x < s.col + s.width &&
                        (s.kind == SPAN_LINK || s.kind == SPAN_FIELD)) {
                        // The link's first span is what holds the focus.
                        int f = i;
                        while (s.kind == SPAN_LINK && f > 0 && !focusable(f)) f--;
                        g_focus = f;
                        activate();
                        break;
                    }
                }
            }
            continue;
        }
        int k = e.key;
        if (k == 3) break;                                          // Ctrl+C
        if (k == 12) {                                              // Ctrl+L
            char url[URL_MAX];
            if (ask_address(url, sizeof(url))) go(url, nullptr);
            continue;
        }
        if (k == FW_KEY_UP)   { move(-1); continue; }
        if (k == FW_KEY_DOWN || k == 9) { move(1); continue; }      // Tab too
        if (k == FW_KEY_PGDN) { page(1); continue; }
        if (k == FW_KEY_PGUP) { page(-1); continue; }
        if (k == 13 || k == 10) { activate(); continue; }

        if (focus_is_text()) {
            Field &f = g_pg.fields[g_pg.spans[g_focus].ref];
            uint32_t n = (uint32_t)strlen(f.value);
            if (k == 127 || k == 8) { if (n) f.value[n - 1] = 0; continue; }
            if (k == 21) { f.value[0] = 0; continue; }                // Ctrl+U
            if (k >= 32 && k < 127 && n + 1 < sizeof(f.value)) { f.value[n] = (char)k; f.value[n + 1] = 0; continue; }
            if (k == FW_KEY_ESC) { move(1); continue; }               // out of the box
            continue;
        }
        if (k == 'q' || k == FW_KEY_ESC) break;
        if (k == FW_KEY_LEFT || k == 'b' || k == 127 || k == 8) { back(); continue; }
        if (k == FW_KEY_RIGHT) { activate(); continue; }
        if (k == ' ') { page(1); continue; }
        if (k == FW_KEY_HOME) { g_top = 0; focus_first_visible(); continue; }
        if (k == FW_KEY_END)  { g_top = (int)g_pg.nlines; scroll_to(g_top); focus_first_visible(); continue; }
        if (k == 'g') {
            char url[URL_MAX];
            if (ask_address(url, sizeof(url))) go(url, nullptr);
            continue;
        }
        if (k == 'r' && strncmp(g_url, "about:", 6)) {
            int top = g_top;
            load(g_url, nullptr);
            g_top = top; scroll_to(g_top); focus_first_visible();
            continue;
        }
        if (k == 'u') { snprintf(g_status, sizeof(g_status), "%s", g_url); continue; }
    }
    fw_tui_end();
    fw_file_remove(BROWSE_FILE);
    return 0;
}

extern "C" int app_main(int) {
    rpc_register_command("browse", "the web, as text", browse_cmd);
    rpc_register_command("www", "the web, as text", browse_cmd);
    return 0;
}
