// HTML to console lines. See browse_page.h.
//
// What it understands is what a text browser needs and no more: text, line and
// paragraph breaks, headings, lists, preformatted text, links, images' alt text,
// and forms (text boxes, passwords, hidden values, check boxes, radio buttons,
// drop-downs, buttons). Scripts, styles and everything they would have done are
// dropped, which is also why a page that builds itself in JavaScript shows little
// or nothing: there is nothing in its HTML to show.
#include "browse_page.h"

#include <string.h>
#include <stdio.h>

enum { S_TEXT, S_TAG, S_COMMENT, S_RAW };

// --- small helpers -------------------------------------------------------------

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static bool ieq(const char *a, const char *b) {
    while (*a && *b) { if (lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
}


static void copy(char *dst, uint32_t cap, const char *src) {
    if (!cap) return;
    uint32_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

uint32_t text_columns(const char *s, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

// Append a code point as UTF-8. Returns bytes written.
static uint32_t put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80)  { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
    }
    out[0] = '?'; return 1;
}

// The screen holds one byte a cell, so text is folded to ASCII as it is laid
// out: accents dropped, typographic quotes and dashes made plain, a few symbols
// spelled out, and anything else shown as '?'. One byte is then one column, and
// what a serial terminal is sent is always whole characters.
static const char *fold_cp(uint32_t cp, char *buf) {
    static const char latin1[] =                          // U+00C0 .. U+00FF
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
        "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp < 0x80) { buf[0] = (char)cp; buf[1] = 0; return buf; }
    if (cp >= 0xC0 && cp <= 0xFF) { buf[0] = latin1[cp - 0xC0]; buf[1] = 0; return buf; }
    switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x2122: return "TM";
    case 0xB0: return "deg";
    case 0xB7: case 0x2022: case 0x2023: case 0x25CF: return "*";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x20AC: return "EUR";
    case 0xA3: return "GBP";
    case 0xA5: return "JPY";
    case 0xA2: return "c";
    case 0xA7: return "S";
    case 0xB6: return "P";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2665: return "<3";
    case 0x200B: case 0x200C: case 0x200D: case 0xAD: case 0xFEFF: return "";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x160: return "S";
    case 0x161: return "s";
    case 0x17D: return "Z";
    case 0x17E: return "z";
    case 0x178: return "Y";
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        // Latin Extended-A: pairs of upper/lower case with accents; the plain
        // letter is close enough to read.
        static const char exta[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        uint32_t i = cp - 0x100;
        if (i < sizeof(exta) - 1) { buf[0] = exta[i]; buf[1] = 0; return buf; }
    }
    return "?";
}

// Fold a UTF-8 string to ASCII in place (it can grow: "EUR"), within `cap`.
static void fold_str(char *s, uint32_t cap) {
    char out[512];
    uint32_t o = 0, i = 0, n = (uint32_t)strlen(s);
    while (i < n && o + 4 < sizeof(out)) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp, len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { cp = '?'; len = 1; }
        bool ok = i + len <= n;
        for (uint32_t k = 1; ok && k < len; k++) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) ok = false; else cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { cp = '?'; len = 1; }
        char b[2];
        const char *f = fold_cp(cp, b);
        while (*f && o + 1 < sizeof(out)) out[o++] = *f++;
        i += len;
    }
    out[o] = 0;
    copy(s, cap, out);
}

// An entity's text, without the & and ;. Writes UTF-8 to `out` (at least 4
// bytes). Returns bytes written, 0 when it is not one this knows.
static uint32_t decode_entity(const char *e, char *out) {
    if (e[0] == '#') {
        uint32_t cp = 0;
        const char *d = e + 1;
        bool hex = (*d == 'x' || *d == 'X');
        if (hex) d++;
        if (!*d) return 0;
        for (; *d; d++) {
            char c = lower(*d);
            uint32_t v;
            if (c >= '0' && c <= '9') v = (uint32_t)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') v = (uint32_t)(c - 'a' + 10);
            else return 0;
            cp = cp * (hex ? 16 : 10) + v;
            if (cp > 0x10FFFF) return 0;
        }
        if (cp == 0xA0) cp = ' ';
        if (cp < 32 && cp != '\t' && cp != '\n') return 0;
        return put_utf8(out, cp);
    }
    static const struct { const char *name; uint32_t cp; } k[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"middot", 0xB7},
        {"bull", 0x2022}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsquo", 0x2018},
        {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"deg", 0xB0},
        {"times", 0xD7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5},
        {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6}, {"larr", 0x2190},
        {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"hearts", 0x2665},
        {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1}, {"agrave", 0xE0},
        {"ouml", 0xF6}, {"uuml", 0xFC}, {"auml", 0xE4}, {"szlig", 0xDF},
        {"ccedil", 0xE7}, {"ntilde", 0xF1}, {"iacute", 0xED}, {"oacute", 0xF3},
        {"uacute", 0xFA}, {"thinsp", ' '}, {"ensp", ' '}, {"emsp", ' '},
        {"zwj", 0}, {"zwnj", 0}, {"shy", 0},
    };
    for (const auto &x : k) {
        if (strcmp(x.name, e) == 0) return x.cp ? put_utf8(out, x.cp) : 0x100;   // 0x100: drop it
    }
    return 0;
}

// --- the string pool ----------------------------------------------------------

static uint16_t pool_add(Page *p, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (p->pool_len + n + 1 > PAGE_POOL) { p->truncated = true; return 0xFFFF; }
    uint16_t at = (uint16_t)p->pool_len;
    memcpy(p->pool + at, s, n + 1);
    p->pool_len += n + 1;
    return at;
}

const char *page_str(const Page *p, uint16_t off) {
    return off == 0xFFFF || off >= p->pool_len ? "" : p->pool + off;
}

static PageStoreWrite g_sw;
static PageStoreRead  g_sr;
static void          *g_sctx;

void page_store(PageStoreWrite w, PageStoreRead r, void *ctx) { g_sw = w; g_sr = r; g_sctx = ctx; }

// What is buffered goes to the store. False once the store has refused.
static bool flush_text(Page *p) {
    if (!p->wbuf_len) return true;
    uint32_t at = p->text_len - p->wbuf_len;
    bool ok = g_sw && g_sw(g_sctx, at, p->wbuf, p->wbuf_len);
    p->wbuf_len = 0;
    if (!ok) p->truncated = true;
    return ok;
}

const char *page_line(const Page *p, uint32_t i, uint32_t *len) {
    static char line[PAGE_WIDTH_MAX + 1];
    *len = 0;
    line[0] = 0;
    if (i >= p->nlines || !g_sr) return line;
    uint32_t want = p->lines[i].len;
    if (want > PAGE_WIDTH_MAX) want = PAGE_WIDTH_MAX;
    uint32_t got = g_sr(g_sctx, p->lines[i].off, line, want);
    line[got] = 0;
    *len = got;
    return line;
}

// --- layout -------------------------------------------------------------------
//
// Words are gathered here and placed whole, so wrapping happens between words.
// A link or a bold run is a span; placing a word while one is open extends the
// span on this line or starts another on the next, which is how a link that
// wraps stays one link.

static char     g_word[96];
static uint32_t g_word_len;
// The word being placed continues the last one with no break between: a word
// longer than g_word arrives in pieces, and the pieces are one word.
static bool     g_word_glue;

static void finish_line(Page *p) {
    if (p->nlines >= PAGE_LINES) { p->truncated = true; p->line_start = p->text_len; p->col = 0; return; }
    p->lines[p->nlines].off = (uint16_t)p->line_start;
    p->lines[p->nlines].len = (uint16_t)(p->text_len - p->line_start);
    p->nlines++;
    p->line_start = p->text_len;
    p->col = 0;
    p->at_line_start = true;
}

static bool last_line_blank(const Page *p) {
    return p->nlines == 0 || p->lines[p->nlines - 1].len == 0;
}

static bool text_put(Page *p, const char *s, uint32_t n) {
    if (p->text_len + n > PAGE_TEXT) { p->truncated = true; return false; }
    while (n) {
        if (p->wbuf_len == PAGE_WBUF && !flush_text(p)) return false;
        uint32_t room = PAGE_WBUF - p->wbuf_len, take = n < room ? n : room;
        memcpy(p->wbuf + p->wbuf_len, s, take);
        p->wbuf_len += take; p->text_len += take; s += take; n -= take;
    }
    return true;
}

// The span a word placed now belongs to, if any.
static void span_cover(Page *p, uint32_t col, uint32_t width) {
    uint8_t kind = 0; uint16_t ref = 0;
    if (p->cur_link >= 0) { kind = SPAN_LINK; ref = (uint16_t)p->cur_link; }
    else if (p->bold)     { kind = SPAN_BOLD; ref = 0; }
    if (!kind) return;
    if (p->nspans) {
        Span &s = p->spans[p->nspans - 1];
        if (s.line == p->nlines && s.kind == kind && s.ref == ref) {
            uint32_t end = col + width;
            if (end > 255) end = 255;
            s.width = (uint8_t)(end - s.col);
            return;
        }
    }
    if (p->nspans >= PAGE_SPANS || col > 255) { p->truncated = true; return; }
    Span &s = p->spans[p->nspans++];
    s.line = (uint16_t)p->nlines; s.col = (uint8_t)col;
    s.width = (uint8_t)(width > 255 ? 255 : width); s.kind = kind; s.ref = ref;
}

static void place_word(Page *p) {
    if (!g_word_len) return;
    uint32_t w = text_columns(g_word, g_word_len);
    uint32_t width = (uint32_t)p->width;
    bool space = p->space_pending && p->col > 0 && !g_word_glue;
    if (!g_word_glue && p->col + (space ? 1 : 0) + w > width && p->col > 0) {
        finish_line(p);
        space = false;
    }
    if (space) {
        // A space inside a link belongs to the link's span.
        bool linked = p->cur_link >= 0 && p->nspans && p->spans[p->nspans - 1].line == p->nlines
                      && p->spans[p->nspans - 1].kind == SPAN_LINK
                      && p->spans[p->nspans - 1].ref == (uint16_t)p->cur_link;
        text_put(p, " ", 1);
        p->col++;
        if (linked) span_cover(p, p->col - 1, 1);
    }
    // A word wider than the page: cut it, a page width at a time.
    uint32_t i = 0;
    while (i < g_word_len) {
        uint32_t room = width - p->col;
        if (!room) { finish_line(p); room = width; }
        uint32_t take = 0, cols = 0;
        while (i + take < g_word_len) {
            unsigned char c = (unsigned char)g_word[i + take];
            bool starts = (c & 0xC0) != 0x80;
            if (starts && cols == room) break;
            if (starts) cols++;
            take++;
        }
        uint32_t at = p->col;
        if (!text_put(p, g_word + i, take)) break;
        p->col += cols;
        span_cover(p, at, cols);
        i += take;
        if (i < g_word_len) finish_line(p);
    }
    g_word_len = 0;
    g_word_glue = false;
    p->space_pending = false;
    p->at_line_start = false;
}

static void flush_word(Page *p) { place_word(p); }

static void add_char(Page *p, char c) {
    if (g_word_len >= sizeof(g_word) - 4) {
        // Keep a multi-byte character whole: cut before its lead byte.
        uint32_t cut = g_word_len;
        while (cut > 0 && ((unsigned char)g_word[cut - 1] & 0xC0) == 0x80) cut--;
        if (cut > 0 && ((unsigned char)g_word[cut - 1] & 0x80)) cut--;
        if (cut == 0) cut = g_word_len;
        char rest[8]; uint32_t nrest = g_word_len - cut;
        memcpy(rest, g_word + cut, nrest);
        g_word_len = cut;
        place_word(p);
        memcpy(g_word, rest, nrest);
        g_word_len = nrest;
        g_word_glue = true;
    }
    g_word[g_word_len++] = c;
}

// A line break (n == 1) or a blank line (n == 2), never more than one blank.
static void block_break(Page *p, int n) {
    flush_word(p);
    if (!p->at_line_start || p->col > 0) finish_line(p);
    if (n == 2 && !last_line_blank(p)) finish_line(p);
    p->space_pending = false;
}

static void put_literal(Page *p, const char *s) {
    flush_word(p);
    bool keep = p->space_pending;
    for (; *s; s++) add_char(p, *s);
    p->space_pending = keep;
    flush_word(p);
}

// A field in the text: a run of spaces that the screen draws over.
static void place_field(Page *p, int idx, uint32_t w) {
    flush_word(p);
    if (w < 3) w = 3;
    if (w > (uint32_t)p->width) w = (uint32_t)p->width;
    if (p->col > 0 && p->col + (p->space_pending ? 1 : 0) + w > (uint32_t)p->width) finish_line(p);
    else if (p->space_pending && p->col > 0) { text_put(p, " ", 1); p->col++; }
    if (p->nspans >= PAGE_SPANS) { p->truncated = true; return; }
    uint32_t at = p->col;
    for (uint32_t i = 0; i < w; i++) if (!text_put(p, " ", 1)) return;
    p->col += w;
    Span &s = p->spans[p->nspans++];
    s.line = (uint16_t)p->nlines; s.col = (uint8_t)(at > 255 ? 255 : at);
    s.width = (uint8_t)w; s.kind = SPAN_FIELD; s.ref = (uint16_t)idx;
    p->fields[idx].width = (uint8_t)w;
    p->space_pending = true;
    p->at_line_start = false;
}

// --- attributes ---------------------------------------------------------------

static void decode_into(const char *s, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n && o + 5 < cap; i++) {
        if (s[i] == '&') {
            char e[12]; uint32_t k = 0, j = i + 1;
            while (j < n && k < sizeof(e) - 1 && s[j] != ';' && s[j] != '&' && !is_space(s[j])) e[k++] = s[j++];
            e[k] = 0;
            char u[4];
            uint32_t m = (k && j < n && s[j] == ';') ? decode_entity(e, u) : 0;
            if (m == 0x100) { i = j; continue; }
            if (m) { memcpy(out + o, u, m); o += m; i = j; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
}

// The tag's name, lower case, into `name`. Returns where the attributes start.
static const char *tag_name(const char *tag, char *name, uint32_t cap) {
    const char *t = tag;
    uint32_t n = 0;
    if (*t == '/') t++;
    while (*t && !is_space(*t) && *t != '/' && *t != '>' && n + 1 < cap) name[n++] = lower(*t++);
    name[n] = 0;
    while (*t && !is_space(*t) && *t != '/') t++;   // a name longer than cap
    return t;
}

// An attribute's value, entities decoded. False when the tag has no such
// attribute; an attribute with no value gives "".
static bool get_attr(const char *attrs, const char *want, char *out, uint32_t cap) {
    const char *t = attrs;
    out[0] = 0;
    while (*t) {
        while (*t && (is_space(*t) || *t == '/')) t++;
        if (!*t) break;
        const char *ns = t;
        while (*t && !is_space(*t) && *t != '=' && *t != '>' && *t != '/') t++;
        uint32_t nlen = (uint32_t)(t - ns);
        while (is_space(*t)) t++;
        const char *vs = t; uint32_t vlen = 0;
        if (*t == '=') {
            t++;
            while (is_space(*t)) t++;
            if (*t == '"' || *t == '\'') {
                char q = *t++;
                vs = t;
                while (*t && *t != q) t++;
                vlen = (uint32_t)(t - vs);
                if (*t) t++;
            } else {
                vs = t;
                while (*t && !is_space(*t) && *t != '>') t++;
                vlen = (uint32_t)(t - vs);
            }
        }
        if (nlen == strlen(want)) {
            bool same = true;
            for (uint32_t i = 0; i < nlen; i++) if (lower(ns[i]) != want[i]) { same = false; break; }
            if (same) { decode_into(vs, vlen, out, cap); fold_str(out, cap); return true; }
        }
        if (t == ns) t++;    // never stall on a stray character
    }
    return false;
}

// --- tags ---------------------------------------------------------------------

static bool one_of(const char *name, const char *const *list) {
    for (; *list; list++) if (!strcmp(name, *list)) return true;
    return false;
}

static const char *const kSkip[]  = {"script", "style", "template", "svg", "math", "iframe", "object", "canvas", nullptr};
static const char *const kBlock[] = {"div", "section", "article", "header", "footer", "nav", "main", "aside",
                                     "address", "figure", "figcaption", "center", "dl", "dt", "dd",
                                     "table", "tbody", "thead", "tfoot", "tr", "caption", "fieldset",
                                     "legend", "details", "summary", "noscript", "body", "html", nullptr};
static const char *const kPara[]  = {"p", "blockquote", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6",
                                     "pre", "form", nullptr};

static int new_field(Page *p, uint8_t type, const char *attrs) {
    if (p->nfields >= PAGE_FIELDS) { p->truncated = true; return -1; }
    int idx = (int)p->nfields++;
    Field &f = p->fields[idx];
    memset(&f, 0, sizeof(f));
    f.type = type;
    f.form = p->cur_form >= 0 ? (uint8_t)p->cur_form : 0xFF;
    char tmp[URL_MAX];
    f.name = get_attr(attrs, "name", tmp, sizeof(tmp)) && tmp[0] ? pool_add(p, tmp) : 0xFFFF;
    if (get_attr(attrs, "value", tmp, sizeof(tmp))) copy(f.value, sizeof(f.value), tmp);
    f.checked = get_attr(attrs, "checked", tmp, sizeof(tmp));
    return idx;
}

static void on_input(Page *p, const char *attrs) {
    char type[24];
    if (!get_attr(attrs, "type", type, sizeof(type))) type[0] = 0;
    for (char *c = type; *c; c++) *c = lower(*c);
    uint8_t t = FIELD_TEXT;
    if (!strcmp(type, "hidden")) t = FIELD_HIDDEN;
    else if (!strcmp(type, "password")) t = FIELD_PASSWORD;
    else if (!strcmp(type, "submit") || !strcmp(type, "image") || !strcmp(type, "button")) t = FIELD_SUBMIT;
    else if (!strcmp(type, "checkbox")) t = FIELD_CHECKBOX;
    else if (!strcmp(type, "radio")) t = FIELD_RADIO;
    else if (!strcmp(type, "reset") || !strcmp(type, "file")) return;   // nothing to do here
    int idx = new_field(p, t, attrs);
    if (idx < 0) return;
    Field &f = p->fields[idx];
    if (t == FIELD_HIDDEN) return;
    uint32_t w = 20;
    char sz[12];
    if (t == FIELD_SUBMIT) {
        if (!f.value[0]) {
            char alt[FIELD_VALUE];
            if (get_attr(attrs, "alt", alt, sizeof(alt)) && alt[0]) copy(f.value, sizeof(f.value), alt);
        }
        w = text_columns(f.value[0] ? f.value : "Submit", (uint32_t)strlen(f.value[0] ? f.value : "Submit")) + 4;
    } else if (t == FIELD_CHECKBOX || t == FIELD_RADIO) {
        w = 3;
    } else if (get_attr(attrs, "size", sz, sizeof(sz))) {
        int v = 0;
        for (char *c = sz; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
        if (v > 0) w = (uint32_t)(v < 8 ? 8 : v > 40 ? 40 : v) + 2;
    }
    place_field(p, idx, w);
}

// The option being read ends: the first, or the one marked selected, becomes
// the drop-down's value (what is sent, then NUL, then the text shown).
static void end_option(Page *p) {
    if (p->in_option == 1 && p->in_select >= 0) {
        Field &f = p->fields[p->in_select];
        if (!p->select_has_value || p->option_selected) {
            const char *v = p->option_has_value ? p->option_value : p->option_text;
            uint32_t vl = (uint32_t)strlen(v);
            if (vl > sizeof(f.value) / 2) vl = sizeof(f.value) / 2;
            memcpy(f.value, v, vl); f.value[vl] = 0;
            copy(f.value + vl + 1, sizeof(f.value) - vl - 1, p->option_text);
            p->select_has_value = true;
        }
    }
    if (p->in_option == 1) p->in_option = 0;
}

static void start_link(Page *p, const char *attrs) {
    char href[URL_MAX];
    if (!get_attr(attrs, "href", href, sizeof(href)) || !href[0]) return;
    if (p->nlinks >= PAGE_SPANS) { p->truncated = true; return; }
    uint16_t off = pool_add(p, href);
    if (off == 0xFFFF) return;
    p->links[p->nlinks] = off;
    p->cur_link = (int)p->nlinks++;
}

static void on_tag(Page *p) {
    p->tag[p->tag_len] = 0;
    const char *tag = p->tag;
    bool closing = tag[0] == '/';
    char name[16];
    const char *attrs = tag_name(tag, name, sizeof(name));
    if (!name[0] || name[0] == '!' || name[0] == '?') return;   // <!DOCTYPE>, <?xml?>

    if (!closing && one_of(name, kSkip)) {
        copy(p->skip_until, sizeof(p->skip_until), name);
        p->state = S_RAW;
        p->dash_run = 0;
        return;
    }
    if (!strcmp(name, "head")) { p->skip = closing ? 0 : 1; return; }
    if (!strcmp(name, "body")) p->skip = 0;     // a page that never closed <head>
    if (!strcmp(name, "title")) { p->in_title = closing ? 0 : 1; if (!closing) p->title_len = 0; return; }
    if (p->skip && !p->in_title) return;

    // Inside a text area, everything up to its end is its content.
    if (!strcmp(name, "textarea")) {
        if (!closing) {
            flush_word(p);
            p->in_textarea = new_field(p, FIELD_TEXTAREA, attrs);
            if (p->in_textarea >= 0) p->fields[p->in_textarea].value[0] = 0;
        } else if (p->in_textarea >= 0) {
            place_field(p, p->in_textarea, 30);
            p->in_textarea = -1;
        }
        return;
    }
    if (!strcmp(name, "button")) {
        if (!closing) {
            char type[16];
            if (get_attr(attrs, "type", type, sizeof(type)) && (ieq(type, "reset") || ieq(type, "button"))) {
                p->in_textarea = -2;     // swallow its text, make nothing
                return;
            }
            flush_word(p);
            p->in_textarea = new_field(p, FIELD_SUBMIT, attrs);
            if (p->in_textarea >= 0) {
                Field &f = p->fields[p->in_textarea];
                // Its label is its text; its value (what is sent) stays in value
                // only when it had one, so keep the label apart.
                if (!get_attr(attrs, "value", f.value, sizeof(f.value))) f.value[0] = 0;
                p->option_text_len = 0; p->option_text[0] = 0;
                p->in_option = 2;        // collect the label into option_text
            }
        } else {
            if (p->in_textarea >= 0) {
                Field &f = p->fields[p->in_textarea];
                const char *label = p->option_text_len ? p->option_text : (f.value[0] ? f.value : "Submit");
                // value keeps what is sent; when there was none, the label is it.
                if (!f.value[0]) copy(f.value, sizeof(f.value), label);
                uint32_t w = text_columns(label, (uint32_t)strlen(label)) + 4;
                place_field(p, p->in_textarea, w);
            }
            p->in_textarea = -1;
            p->in_option = 0;
        }
        return;
    }
    if (!strcmp(name, "select")) {
        if (!closing) {
            flush_word(p);
            p->in_select = new_field(p, FIELD_SELECT, attrs);
            p->select_has_value = false;
        } else if (p->in_select >= 0) {
            end_option(p);              // the last <option> is often left open
            Field &f = p->fields[p->in_select];
            uint32_t vlen = (uint32_t)strlen(f.value);
            const char *shown = vlen + 1 < sizeof(f.value) ? f.value + vlen + 1 : "";
            uint32_t w = text_columns(shown, (uint32_t)strlen(shown)) + 4;
            place_field(p, p->in_select, w > 24 ? 24 : w);
            p->in_select = -1;
        }
        return;
    }
    if (!strcmp(name, "option")) {
        end_option(p);
        if (!closing && p->in_select >= 0) {
            char tmp[12];
            p->option_has_value = get_attr(attrs, "value", p->option_value, sizeof(p->option_value));
            p->option_selected = get_attr(attrs, "selected", tmp, sizeof(tmp));
            p->option_text_len = 0; p->option_text[0] = 0;
            p->in_option = 1;
        }
        return;
    }
    if (p->in_select >= 0) return;          // nothing else inside a drop-down

    if (!strcmp(name, "a")) {
        flush_word(p);
        if (closing) p->cur_link = -1;
        else { p->cur_link = -1; start_link(p, attrs); }
        return;
    }
    if (!strcmp(name, "br")) {
        flush_word(p);
        if (p->col == 0 && !p->at_line_start) finish_line(p);
        finish_line(p);
        return;
    }
    if (!strcmp(name, "hr")) {
        block_break(p, 1);
        char dash[PAGE_WIDTH_MAX + 1];
        int n = p->width > 40 ? 40 : p->width;
        memset(dash, '-', (size_t)n); dash[n] = 0;
        put_literal(p, dash);
        block_break(p, 1);
        return;
    }
    if (!strcmp(name, "img")) {
        char alt[80];
        if (get_attr(attrs, "alt", alt, sizeof(alt)) && alt[0]) {
            char shown[84];
            snprintf(shown, sizeof(shown), "[%s]", alt);
            // Through the word buffer so it wraps and joins a link around it.
            flush_word(p);
            for (const char *c = shown; *c; c++) {
                if (*c == ' ') { flush_word(p); p->space_pending = true; }
                else add_char(p, *c);
            }
            flush_word(p);
        }
        return;
    }
    if (!strcmp(name, "input")) { if (!closing) on_input(p, attrs); return; }
    if (!strcmp(name, "form")) {
        block_break(p, 1);
        if (closing) { p->cur_form = -1; return; }
        if (p->nforms >= PAGE_FORMS) { p->truncated = true; p->cur_form = -1; return; }
        Form &f = p->forms[p->nforms];
        char tmp[URL_MAX];
        f.action = get_attr(attrs, "action", tmp, sizeof(tmp)) && tmp[0] ? pool_add(p, tmp) : 0xFFFF;
        f.post = get_attr(attrs, "method", tmp, sizeof(tmp)) && ieq(tmp, "post");
        p->cur_form = (int)p->nforms++;
        return;
    }
    if (!strcmp(name, "li")) {
        if (closing) return;
        block_break(p, 1);
        int depth = p->list_depth > 0 ? p->list_depth : 1;
        char lead[24]; int n = 0;
        for (int i = 1; i < depth && n < 16; i++) { lead[n++] = ' '; lead[n++] = ' '; }
        lead[n++] = '*'; lead[n] = 0;
        // As text, so the line wraps under it like any other.
        for (int i = 0; i < n; i++) {
            if (lead[i] == ' ') { text_put(p, " ", 1); p->col++; }
            else add_char(p, lead[i]);
        }
        flush_word(p);
        p->space_pending = true;
        return;
    }
    if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
        if (closing) { if (p->list_depth) p->list_depth--; }
        else p->list_depth++;
        block_break(p, p->list_depth > (closing ? 0 : 1) ? 1 : 2);
        return;
    }
    if (!strcmp(name, "td") || !strcmp(name, "th")) {
        flush_word(p);
        if (!closing && p->col > 0) p->space_pending = true;
        return;
    }
    if (!strcmp(name, "pre")) {
        block_break(p, 2);
        p->pre = closing ? 0 : 1;
        return;
    }
    bool head = name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2];
    if (head || !strcmp(name, "b") || !strcmp(name, "strong")) {
        if (head) block_break(p, 2);
        else flush_word(p);
        bool strong = !head || name[1] <= '3';
        if (strong) {
            if (closing) { if (p->bold) p->bold--; }
            else p->bold++;
        }
        return;
    }
    if (one_of(name, kPara)) { block_break(p, 2); return; }
    if (one_of(name, kBlock)) { block_break(p, 1); return; }
    // Anything else (span, em, i, code, small, label, font...) is inline and
    // changes nothing here.
}

// --- text ---------------------------------------------------------------------

static void text_ascii(Page *p, char c) {
    if (p->in_title) {
        if (p->title_len + 1 < sizeof(p->title)) {
            if (is_space(c)) { if (p->title_len && p->title[p->title_len - 1] != ' ') p->title[p->title_len++] = ' '; }
            else p->title[p->title_len++] = c;
            p->title[p->title_len] = 0;
        }
        return;
    }
    if (p->skip) return;
    if (p->in_option) {
        if (p->option_text_len + 1 < sizeof(p->option_text)) {
            if (is_space(c)) { if (p->option_text_len && p->option_text[p->option_text_len - 1] != ' ') p->option_text[p->option_text_len++] = ' '; }
            else p->option_text[p->option_text_len++] = c;
            p->option_text[p->option_text_len] = 0;
        }
        return;
    }
    if (p->in_textarea >= 0) {
        Field &f = p->fields[p->in_textarea];
        uint32_t n = (uint32_t)strlen(f.value);
        if (n + 1 < sizeof(f.value) && (c != '\n' || n)) { f.value[n] = c == '\n' ? ' ' : c; f.value[n + 1] = 0; }
        return;
    }
    if (p->in_textarea == -2 || p->in_select >= 0) return;
    if (p->pre) {
        if (c == '\n') { flush_word(p); finish_line(p); return; }
        if (c == '\r') return;
        if (c == ' ' || c == '\t') {
            flush_word(p);
            uint32_t n = c == '\t' ? 8 - (p->col % 8) : 1;
            for (uint32_t i = 0; i < n && p->col < (uint32_t)p->width; i++) { text_put(p, " ", 1); p->col++; }
            p->at_line_start = false;
            return;
        }
        add_char(p, c);
        return;
    }
    if (is_space(c)) { flush_word(p); p->space_pending = true; return; }
    add_char(p, c);
}

// A byte of the page's text: UTF-8 is gathered into a character and folded.
static void text_byte(Page *p, char ch) {
    unsigned char c = (unsigned char)ch;
    if (c < 0x80) {
        if (p->utf8_left) { p->utf8_left = 0; text_ascii(p, '?'); }   // a sequence cut short
        text_ascii(p, ch);
        return;
    }
    if ((c & 0xC0) == 0x80) {
        if (!p->utf8_left) { text_ascii(p, '?'); return; }           // a stray continuation
        p->utf8_cp = (p->utf8_cp << 6) | (c & 0x3F);
        if (--p->utf8_left == 0) {
            char b[2];
            for (const char *f = fold_cp(p->utf8_cp, b); *f; f++) text_ascii(p, *f);
        }
        return;
    }
    if (p->utf8_left) text_ascii(p, '?');
    if ((c & 0xE0) == 0xC0)      { p->utf8_cp = c & 0x1F; p->utf8_left = 1; }
    else if ((c & 0xF0) == 0xE0) { p->utf8_cp = c & 0x0F; p->utf8_left = 2; }
    else if ((c & 0xF8) == 0xF0) { p->utf8_cp = c & 0x07; p->utf8_left = 3; }
    else { p->utf8_left = 0; text_ascii(p, '?'); }
}

static void text_decoded(Page *p, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) text_byte(p, s[i]);
}

static void end_entity(Page *p, bool had_semicolon) {
    p->ent[p->ent_len] = 0;
    char u[4];
    uint32_t m = had_semicolon && p->ent_len ? decode_entity(p->ent, u) : 0;
    if (m == 0x100) { p->ent_len = 0; return; }
    if (m) { text_decoded(p, u, m); p->ent_len = 0; return; }
    // Not one: the text as it was.
    text_byte(p, '&');
    text_decoded(p, p->ent, p->ent_len);
    if (had_semicolon) text_byte(p, ';');
    p->ent_len = 0;
}

// --- the tokenizer ------------------------------------------------------------

void page_begin(Page *p, int width) {
    memset(p, 0, sizeof(*p));
    if (width < 20) width = 20;
    if (width > PAGE_WIDTH_MAX) width = PAGE_WIDTH_MAX;
    p->width = width;
    p->cur_link = p->cur_form = p->in_textarea = p->in_select = -1;
    p->at_line_start = true;
    p->state = S_TEXT;
    p->ent_len = 0xFFFFFFFF;   // not inside an entity
    g_word_len = 0;
    g_word_glue = false;
}

void page_feed(Page *p, const char *data, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == 0) continue;
        switch (p->state) {
        case S_TEXT:
            if (p->ent_len != 0xFFFFFFFF) {
                bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#';
                if (c == ';') { end_entity(p, true); p->ent_len = 0xFFFFFFFF; continue; }
                if (word && p->ent_len < sizeof(p->ent) - 1) { p->ent[p->ent_len++] = c; continue; }
                end_entity(p, false);
                p->ent_len = 0xFFFFFFFF;
                // and c is handled below as ordinary text
            }
            if (c == '<') { p->state = S_TAG; p->tag_len = 0; p->dash_run = 0; continue; }
            if (c == '&') { p->ent_len = 0; continue; }
            text_byte(p, c);
            break;
        case S_TAG: {
            // Quotes are tracked so a '>' inside an attribute value does not end
            // the tag; dash_run holds the open quote character here.
            if (p->dash_run) {
                if (c == (char)p->dash_run) p->dash_run = 0;
            } else if (c == '"' || c == '\'') {
                // Only inside attributes: "a='b'" style. A quote right after the
                // '<' is not a tag at all.
                if (p->tag_len > 0) p->dash_run = (uint32_t)(unsigned char)c;
            } else if (c == '>') {
                p->state = S_TEXT;
                on_tag(p);
                continue;
            }
            if (p->tag_len == 0 && (c == ' ' || c == '=' || (c >= '0' && c <= '9'))) {
                // "a < b", "<3": not a tag. The text it was.
                p->state = S_TEXT;
                text_byte(p, '<');
                text_byte(p, c);
                continue;
            }
            if (p->tag_len < TAG_MAX - 1) p->tag[p->tag_len++] = c;
            if (p->tag_len == 3 && p->tag[0] == '!' && p->tag[1] == '-' && p->tag[2] == '-') {
                p->state = S_COMMENT; p->dash_run = 0;
            }
            break;
        }
        case S_COMMENT:
            if (c == '-') p->dash_run++;
            else if (c == '>' && p->dash_run >= 2) { p->state = S_TEXT; p->dash_run = 0; }
            else p->dash_run = 0;
            break;
        case S_RAW: {
            // Looking for "</name": dash_run counts how much of it has matched.
            uint32_t k = p->dash_run;
            uint32_t nl = (uint32_t)strlen(p->skip_until);
            char want = k == 0 ? '<' : k == 1 ? '/' : p->skip_until[k - 2];
            if (lower(c) == want) {
                p->dash_run = k + 1;
                if (p->dash_run == nl + 2) {
                    // The rest of the closing tag is skipped as a tag.
                    p->state = S_TAG;
                    p->tag_len = 0;
                    p->tag[p->tag_len++] = '/';
                    for (uint32_t j = 0; j < nl; j++) p->tag[p->tag_len++] = p->skip_until[j];
                    p->dash_run = 0;
                }
            } else {
                p->dash_run = (c == '<') ? 1 : 0;
            }
            break;
        }
        }
    }
}

void page_feed_text(Page *p, const char *data, uint32_t n) {
    p->pre = 1;
    for (uint32_t i = 0; i < n; i++) if (data[i]) text_byte(p, data[i]);
}

void page_end(Page *p) {
    if (p->ent_len != 0xFFFFFFFF) { end_entity(p, false); p->ent_len = 0xFFFFFFFF; }
    if (p->utf8_left) { p->utf8_left = 0; text_ascii(p, '?'); }     // cut off at the end
    flush_word(p);
    if (p->col > 0 || !p->at_line_start) finish_line(p);
    // Trailing blank lines say nothing.
    while (p->nlines && p->lines[p->nlines - 1].len == 0) p->nlines--;
    flush_text(p);
    // Title: trim the end.
    while (p->title_len && p->title[p->title_len - 1] == ' ') p->title[--p->title_len] = 0;
}

// --- URLs ---------------------------------------------------------------------

static bool scheme_of(const char *s, char *out, uint32_t cap) {
    uint32_t i = 0;
    while (s[i] && s[i] != ':' && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
    if (s[i] != ':' || i == 0 || i + 1 > cap) return false;
    for (uint32_t j = 0; j < i; j++) out[j] = lower(s[j]);
    out[i] = 0;
    return true;
}

// Where the path starts in an absolute URL (after scheme://host[:port]).
static const char *path_of(const char *url) {
    const char *h = strstr(url, "://");
    if (!h) return url + strlen(url);
    h += 3;
    while (*h && *h != '/' && *h != '?' && *h != '#') h++;
    return h;
}

// Remove "." and ".." segments from the path part of `url`, in place.
static void normalise(char *url) {
    char *path = (char *)path_of(url);
    if (*path != '/') return;
    char *q = path;
    while (*q && *q != '?' && *q != '#') q++;
    char tail[URL_MAX];
    copy(tail, sizeof(tail), q);
    *q = 0;
    // The segments, as (start, length) into the path; ".." pops one.
    const char *seg[64]; uint32_t len[64]; int n = 0;
    bool trailing = false;
    const char *s = path + 1;
    for (;;) {
        const char *e = s;
        while (*e && *e != '/') e++;
        uint32_t l = (uint32_t)(e - s);
        bool last = !*e;
        if (l == 1 && s[0] == '.') { trailing = last; }
        else if (l == 2 && s[0] == '.' && s[1] == '.') { if (n) n--; trailing = last; }
        else if (l == 0 && !last) { /* "//": skip */ }
        else { if (n < 64) { seg[n] = s; len[n] = l; n++; } trailing = false; }
        if (last) break;
        s = e + 1;
    }
    char out[URL_MAX];
    uint32_t o = 0;
    for (int i = 0; i < n && o + len[i] + 2 < sizeof(out); i++) {
        out[o++] = '/';
        memcpy(out + o, seg[i], len[i]); o += len[i];
    }
    if (o == 0 || trailing) out[o++] = '/';
    out[o] = 0;
    uint32_t room = URL_MAX - (uint32_t)(path - url);
    snprintf(path, room, "%s%s", out, tail);
}

static void drop_fragment(char *url) {
    char *h = strchr(url, '#');
    if (h) *h = 0;
}

bool url_resolve(const char *base, const char *href0, char *out, uint32_t cap) {
    while (is_space(*href0)) href0++;
    char href[URL_MAX];
    copy(href, sizeof(href), href0);
    uint32_t hl = (uint32_t)strlen(href);
    while (hl && is_space(href[hl - 1])) href[--hl] = 0;
    char sch[16];
    if (scheme_of(href, sch, sizeof(sch))) {
        if (strcmp(sch, "http") && strcmp(sch, "https")) return false;
        if (strlen(href) + 1 > cap) return false;
        copy(out, cap, href);
        drop_fragment(out);
        normalise(out);
        return true;
    }
    if (!scheme_of(base, sch, sizeof(sch))) return false;
    if (!href[0] || href[0] == '#') {
        copy(out, cap, base);
        drop_fragment(out);
        return true;
    }
    if (href[0] == '/' && href[1] == '/') {
        int n = snprintf(out, cap, "%s:%s", sch, href);
        if (n < 0 || (uint32_t)n >= cap) return false;
        drop_fragment(out);
        normalise(out);
        return true;
    }
    const char *bpath = path_of(base);
    uint32_t origin = (uint32_t)(bpath - base);
    char tmp[URL_MAX * 2];
    if (origin >= sizeof(tmp)) return false;
    memcpy(tmp, base, origin);
    tmp[origin] = 0;
    if (href[0] == '/') {
        snprintf(tmp + origin, sizeof(tmp) - origin, "%s", href);
    } else if (href[0] == '?') {
        uint32_t pl = 0;
        while (bpath[pl] && bpath[pl] != '?' && bpath[pl] != '#') pl++;
        snprintf(tmp + origin, sizeof(tmp) - origin, "%.*s%s", (int)pl, pl ? bpath : "/", href);
    } else {
        // The base's directory: up to and including its last '/' before any query.
        uint32_t pl = 0, last = 0;
        while (bpath[pl] && bpath[pl] != '?' && bpath[pl] != '#') { if (bpath[pl] == '/') last = pl + 1; pl++; }
        if (!last) snprintf(tmp + origin, sizeof(tmp) - origin, "/%s", href);
        else snprintf(tmp + origin, sizeof(tmp) - origin, "%.*s%s", (int)last, bpath, href);
    }
    if (strlen(tmp) + 1 > cap) return false;
    copy(out, cap, tmp);
    drop_fragment(out);
    normalise(out);
    return true;
}

uint32_t url_encode(const char *in, char *out, uint32_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
    return o;
}

#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

void url_from_input(const char *in0, char *out, uint32_t cap) {
    while (is_space(*in0)) in0++;
    char in[URL_MAX];
    copy(in, sizeof(in), in0);
    uint32_t n = (uint32_t)strlen(in);
    while (n && is_space(in[n - 1])) in[--n] = 0;
    if (strstr(in, "://")) { copy(out, cap, in); return; }
    bool space = strchr(in, ' ') != nullptr;
    bool dot = strchr(in, '.') != nullptr;
    bool colon = strchr(in, ':') != nullptr;
    if (space || (!dot && !colon && strcmp(in, "localhost"))) {
        // One word: the site of that name, the way people type "google".
        if (!space && n) {
            bool word = true;
            for (uint32_t i = 0; i < n; i++) {
                char c = lower(in[i]);
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { word = false; break; }
            }
            if (word) { snprintf(out, cap, "https://www.%s.com/", in); return; }
        }
        char q[URL_MAX];
        url_encode(in, q, sizeof(q));
        snprintf(out, cap, SEARCH_URL "%s", q);
        return;
    }
    snprintf(out, cap, "https://%s", in);
    // A bare host gets its root path.
    if (!strchr(out + 8, '/')) {
        uint32_t l = (uint32_t)strlen(out);
        if (l + 1 < cap) { out[l] = '/'; out[l + 1] = 0; }
    }
}

// The host part of an absolute URL, lower case.
static void host_of(const char *url, char *out, uint32_t cap) {
    out[0] = 0;
    const char *h = strstr(url, "://");
    if (!h) return;
    h += 3;
    uint32_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != '?' && h[n] != '#' && h[n] != ':' && n + 1 < cap) { out[n] = lower(h[n]); n++; }
    out[n] = 0;
}

static bool is_google(const char *url) {
    char host[96];
    host_of(url, host, sizeof(host));
    const char *h = host;
    if (!strncmp(h, "www.", 4)) h += 4;
    return !strncmp(h, "google.", 7);
}

static bool add_pair(char *out, uint32_t cap, uint32_t *len, const char *name, const char *value) {
    char en[URL_MAX], ev[URL_MAX];
    url_encode(name, en, sizeof(en));
    url_encode(value, ev, sizeof(ev));
    int n = snprintf(out + *len, cap - *len, "%s%s=%s", *len ? "&" : "", en, ev);
    if (n < 0 || *len + (uint32_t)n >= cap) return false;
    *len += (uint32_t)n;
    return true;
}

int form_submit(const Page *p, const char *base, int form, int pressed,
                char *url, uint32_t url_cap, char *body, uint32_t body_cap,
                char *note, uint32_t note_cap) {
    if (note && note_cap) note[0] = 0;
    if (form < 0 || (uint32_t)form >= p->nforms) return 0;
    const Form &f = p->forms[form];
    char action[URL_MAX];
    if (!url_resolve(base, f.action == 0xFFFF ? "" : page_str(p, f.action), action, sizeof(action))) return 0;

    char q[1024];
    uint32_t ql = 0;
    q[0] = 0;
    const char *search = nullptr;
    for (uint32_t i = 0; i < p->nfields; i++) {
        const Field &fd = p->fields[i];
        if (fd.form != (uint8_t)form || fd.name == 0xFFFF) continue;
        const char *name = page_str(p, fd.name);
        const char *value = fd.value;
        switch (fd.type) {
        case FIELD_SUBMIT:
            if ((int)i != pressed) continue;
            break;
        case FIELD_CHECKBOX: case FIELD_RADIO:
            if (!fd.checked) continue;
            if (!value[0]) value = "on";
            break;
        default:
            break;
        }
        if (!strcmp(name, "q")) search = fd.value;
        if (!add_pair(q, sizeof(q), &ql, name, value)) return 0;
    }

    // Google turns away every browser without JavaScript ("Your browser isn't
    // supported anymore"), so its search box would lead nowhere. The words go to
    // DuckDuckGo's text version instead, and the page says so.
    if (is_google(action) && !strncmp(path_of(action), "/search", 7) && search) {
        char enc[URL_MAX];
        url_encode(search, enc, sizeof(enc));
        snprintf(url, url_cap, SEARCH_URL "%s", enc);
        if (note) snprintf(note, note_cap, "Google needs JavaScript, so this searched DuckDuckGo.");
        return 1;
    }
    if (f.post) {
        copy(url, url_cap, action);
        if (ql + 1 > body_cap) return 0;
        copy(body, body_cap, q);
        return 2;
    }
    // GET: the form's fields replace the action's own query.
    char *qm = strchr(action, '?');
    if (qm) *qm = 0;
    int n = snprintf(url, url_cap, "%s%s%s", action, ql ? "?" : "", q);
    if (n < 0 || (uint32_t)n >= url_cap) return 0;
    return 1;
}
