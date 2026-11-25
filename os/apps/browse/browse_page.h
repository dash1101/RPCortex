// The page: HTML turned into lines of text a console can show, with the links
// and form fields on them.
//
// No firmware calls in here, so the host tests compile it as it is. The HTML
// arrives in pieces (page_feed), from a file read a kilobyte at a time, and is
// laid out as it comes: nothing keeps the source. Everything lives in the one
// Page, sized at compile time, because a package's heap is 12 KB and a page is
// not; what does not fit is dropped, and `truncated` says so.
#ifndef BROWSE_PAGE_H
#define BROWSE_PAGE_H

#include <stdint.h>

#define PAGE_TEXT      65535   // the laid-out text, every line end to end (in the store)
#define PAGE_WBUF      2048    // what is held in RAM before it goes to the store
#define PAGE_LINES     1600
#define PAGE_SPANS     450     // links and fields on the lines, and bold runs
#define PAGE_POOL      5120    // link targets, form actions, field names
#define PAGE_FORMS     10
#define PAGE_FIELDS    32
#define FIELD_VALUE    112
#define PAGE_TITLE     96
#define URL_MAX        512
#define TAG_MAX        640     // one tag, attributes and all; longer is cut
#define PAGE_WIDTH_MAX 200

enum SpanKind : uint8_t { SPAN_LINK = 1, SPAN_FIELD = 2, SPAN_BOLD = 3, SPAN_HEAD = 4 };

enum FieldType : uint8_t {
    FIELD_TEXT, FIELD_PASSWORD, FIELD_HIDDEN, FIELD_SUBMIT, FIELD_CHECKBOX,
    FIELD_RADIO, FIELD_SELECT, FIELD_TEXTAREA
};

struct Line { uint16_t off, len; };

struct Span {
    uint16_t line;
    uint8_t  col;              // in columns, not bytes
    uint8_t  width;            // columns
    uint8_t  kind;             // SpanKind
    uint16_t ref;              // link index, or field index
};

struct Form {
    uint16_t action;           // pool offset; 0xFFFF when the form had none
    bool     post;
};

struct Field {
    uint8_t  type;             // FieldType
    uint8_t  form;             // 0xFF: outside any form
    uint8_t  width;            // columns shown
    bool     checked;
    uint16_t name;             // pool offset; 0xFFFF for none
    char     value[FIELD_VALUE];
};

struct Page {
    // --- the result
    char     wbuf[PAGE_WBUF];      // text not yet handed to the store
    uint32_t wbuf_len;
    uint32_t text_len;             // all of it, stored and not
    Line     lines[PAGE_LINES];
    uint32_t nlines;
    Span     spans[PAGE_SPANS];
    uint32_t nspans;
    char     pool[PAGE_POOL];
    uint32_t pool_len;
    uint16_t links[PAGE_SPANS];    // pool offset of each link's target
    uint32_t nlinks;
    Form     forms[PAGE_FORMS];
    uint32_t nforms;
    Field    fields[PAGE_FIELDS];
    uint32_t nfields;
    char     title[PAGE_TITLE];
    bool     truncated;            // something did not fit

    // --- layout state, while feeding
    int      width;
    uint32_t col;                  // columns used on the line being built
    uint32_t line_start;           // text offset of that line
    bool     space_pending;        // a run of whitespace has been seen
    bool     at_line_start;
    int      blank_wanted;         // 0 none, 1 line break, 2 blank line
    int      pre;                  // inside <pre>
    int      bold;                 // inside b/strong/h1..h3
    int      list_depth;
    int      cur_link;             // -1: not inside <a href>
    int      cur_form;             // -1: not inside a <form>
    int      skip;                 // inside script/style/head-ish: drop text
    int      in_title;
    uint32_t title_len;
    int      in_textarea;          // field index, or -1
    int      in_select;            // field index, or -1
    int      in_option;
    bool     option_selected;
    bool     select_has_value;
    char     option_value[FIELD_VALUE];
    bool     option_has_value;
    uint32_t option_text_len;
    char     option_text[FIELD_VALUE];

    // --- tokenizer state
    int      state;
    char     tag[TAG_MAX];
    uint32_t tag_len;
    char     ent[12];
    uint32_t ent_len;
    char     skip_until[12];       // "script" while skipping one
    uint32_t dash_run;             // comment end detection
    uint32_t utf8_left;            // continuation bytes still to come
    uint32_t utf8_cp;              // the character they belong to
};

// Where the laid-out text lives. It is not kept in RAM: a page of text is
// tens of kilobytes, and a package's data has to fit in one piece of a heap
// that a long-running board has broken up. The package keeps it in a file; the
// host tests in a buffer. `write` appends `n` bytes at `off` (0 starts afresh);
// `read` returns how many of `n` bytes at `off` it gave.
typedef bool     (*PageStoreWrite)(void *ctx, uint32_t off, const char *data, uint32_t n);
typedef uint32_t (*PageStoreRead)(void *ctx, uint32_t off, char *buf, uint32_t n);
void page_store(PageStoreWrite w, PageStoreRead r, void *ctx);

// Start a page `width` columns wide.
void page_begin(Page *p, int width);
// Hand over the next `n` bytes of HTML. Any split is fine, mid-tag included.
void page_feed(Page *p, const char *data, uint32_t n);
// The end of the source: finish the line being built.
void page_end(Page *p);
// Plain text (not HTML), laid out the same way: for text/plain responses.
void page_feed_text(Page *p, const char *data, uint32_t n);

// A line's text, `len` bytes long, terminated. Read from the store into one
// buffer, so it is good until the next call.
const char *page_line(const Page *p, uint32_t i, uint32_t *len);
// A pool string (link target, form action, field name).
const char *page_str(const Page *p, uint16_t off);

// --- URLs ----------------------------------------------------------------------

// `href` read against `base`, into `out`. Fragments are dropped. False when the
// result is not http(s) (mailto:, javascript:) or does not fit.
bool url_resolve(const char *base, const char *href, char *out, uint32_t cap);
// What someone typed into the address bar: a URL, a host, or words to search.
void url_from_input(const char *in, char *out, uint32_t cap);
// Percent-encode for a query string (spaces as +).
uint32_t url_encode(const char *in, char *out, uint32_t cap);

// A form's submission. `pressed` is the submit button used, or -1. Fills
// `url` (and, for a POST, `body`). Returns 1 GET, 2 POST, 0 failure.
// `note` gets a line to show when the request was redirected somewhere else
// (Google's search, which refuses browsers without JavaScript).
int form_submit(const Page *p, const char *base, int form, int pressed,
                char *url, uint32_t url_cap, char *body, uint32_t body_cap,
                char *note, uint32_t note_cap);

// Columns in `s[0..n)`. The page holds ASCII only (UTF-8 is folded as it is
// laid out), so this is n for anything it produced; it still counts UTF-8
// properly for text from elsewhere.
uint32_t text_columns(const char *s, uint32_t n);

#endif
