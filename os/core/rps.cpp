#include "rps.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// --- variables --------------------------------------------------------------

struct RpsVars {
    char name[RPS_VARS][RPS_NAME_MAX];
    char val[RPS_VARS][RPS_VAL_MAX];
    int  n;
};

static const char *var_get(const RpsVars *v, const char *name, uint32_t len) {
    for (int i = 0; i < v->n; i++)
        if (strlen(v->name[i]) == len && !strncmp(v->name[i], name, len)) return v->val[i];
    return nullptr;
}

static bool var_set(RpsVars *v, const char *name, const char *val) {
    for (int i = 0; i < v->n; i++)
        if (!strcmp(v->name[i], name)) { snprintf(v->val[i], RPS_VAL_MAX, "%s", val); return true; }
    if (v->n >= RPS_VARS) return false;
    snprintf(v->name[v->n], RPS_NAME_MAX, "%s", name);
    snprintf(v->val[v->n], RPS_VAL_MAX, "%s", val);
    v->n++;
    return true;
}

// A name character. Ends a $NAME at anything else, so "$a/$b" and "$a." work.
static bool name_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

uint32_t rps_expand(const RpsVars *v, const char *in, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; ) {
        if (*p != '$') { out[o++] = *p++; continue; }
        p++;
        // "$$" is a literal dollar; without it a script could never print one.
        if (*p == '$') { out[o++] = '$'; p++; continue; }
        const char *s = p;
        while (name_ch(*p)) p++;
        uint32_t len = (uint32_t)(p - s);
        if (!len) { out[o++] = '$'; continue; }
        const char *val = var_get(v, s, len);
        // An unset variable expands to NOTHING rather than to its own name.
        // That is v1's behaviour, and it makes "if empty $x" work before x is
        // ever set.
        if (val) for (const char *q = val; *q && o + 1 < cap; q++) out[o++] = *q;
    }
    out[o] = 0;
    return o;
}

// --- small string helpers ---------------------------------------------------

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

// Copy the next whitespace-separated word, honouring quotes so a value with
// spaces survives. Returns where it stopped.
static const char *word(const char *p, char *out, uint32_t cap) {
    p = skip_ws(p);
    uint32_t n = 0;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q) { if (n + 1 < cap) out[n++] = *p; p++; }
        if (*p == q) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < cap) out[n++] = *p; p++; }
    }
    out[n] = 0;
    return p;
}

static bool is_num(const char *s, long *out) {
    if (!*s) return false;
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || *end) return false;
    *out = v;
    return true;
}

// v1's rule: numeric when BOTH sides look like numbers, lexical otherwise. So
// "10" > "9" but "b" > "a", and "10" vs "abc" compares as text.
static int compare(const char *a, const char *b) {
    long x, y;
    if (is_num(a, &x) && is_num(b, &y)) return x < y ? -1 : (x > y ? 1 : 0);
    int c = strcmp(a, b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// --- lines ------------------------------------------------------------------

struct Script {
    const char *text;
    uint32_t    line_start[512];   // byte offset of each line
    uint32_t    lines;
};

static void index_lines(Script *s, const char *text) {
    s->text = text;
    s->lines = 0;
    if (!text) return;
    s->line_start[s->lines++] = 0;
    for (uint32_t i = 0; text[i] && s->lines < 512; i++)
        if (text[i] == '\n') s->line_start[s->lines++] = i + 1;
}

static void get_line(const Script *s, uint32_t idx, char *out, uint32_t cap) {
    out[0] = 0;
    if (idx >= s->lines) return;
    const char *p = s->text + s->line_start[idx];
    uint32_t n = 0;
    while (*p && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    while (n && (out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    out[n] = 0;
}

// --- blocks -----------------------------------------------------------------

enum BlockKind { BLK_IF, BLK_WHILE };

struct Block {
    uint8_t  kind;
    uint32_t head;        // the `if` or `while` line
    bool     taken;       // an if whose branch ran, so `else` is skipped
};

// Find the line closing the block that opens at `from`, and optionally the
// `else` that belongs to it. Nesting is counted so an inner block's `end` is
// not mistaken for this one's.
static bool find_close(const Script *s, uint32_t from, uint32_t *end_line, uint32_t *else_line) {
    int depth = 0;
    if (else_line) *else_line = 0;
    char buf[RPS_LINE_MAX], w[24];
    for (uint32_t i = from; i < s->lines; i++) {
        get_line(s, i, buf, sizeof(buf));
        word(buf, w, sizeof(w));
        if (!strcmp(w, "if") || !strcmp(w, "while")) depth++;
        else if (!strcmp(w, "end")) {
            depth--;
            if (depth == 0) { *end_line = i; return true; }
        } else if (!strcmp(w, "else") && depth == 1 && else_line && !*else_line) {
            *else_line = i;
        }
    }
    return false;
}

// --- conditions -------------------------------------------------------------

struct Ctx {
    RpsVars      vars;
    const RpsHost *host;
    RpsResult    *res;
};

static bool eval_cond(Ctx *c, const char *expr, bool *out);

static bool eval_cond(Ctx *c, const char *expr, bool *out) {
    char op[16];
    const char *p = word(expr, op, sizeof(op));

    if (!strcmp(op, "not")) {
        bool inner = false;
        if (!eval_cond(c, p, &inner)) return false;
        *out = !inner;
        return true;
    }

    char a[RPS_VAL_MAX], b[RPS_VAL_MAX];
    if (!strcmp(op, "eq") || !strcmp(op, "ne") || !strcmp(op, "gt") ||
        !strcmp(op, "lt") || !strcmp(op, "ge") || !strcmp(op, "le")) {
        p = word(p, a, sizeof(a));
        word(p, b, sizeof(b));
        int cmp = compare(a, b);
        if      (!strcmp(op, "eq")) *out = cmp == 0;
        else if (!strcmp(op, "ne")) *out = cmp != 0;
        else if (!strcmp(op, "gt")) *out = cmp > 0;
        else if (!strcmp(op, "lt")) *out = cmp < 0;
        else if (!strcmp(op, "ge")) *out = cmp >= 0;
        else                        *out = cmp <= 0;
        return true;
    }
    if (!strcmp(op, "contains")) {
        p = word(p, a, sizeof(a));
        word(p, b, sizeof(b));
        *out = strstr(a, b) != nullptr;
        return true;
    }
    if (!strcmp(op, "exists")) {
        word(p, a, sizeof(a));
        *out = c->host->exists ? c->host->exists(c->host->ctx, a) : false;
        return true;
    }
    if (!strcmp(op, "empty")) {
        word(p, a, sizeof(a));
        *out = a[0] == 0;
        return true;
    }

    // Anything else is a command, and its success is the condition. That is what
    // makes `if ping -c 1 host` work without the language knowing about ping.
    *out = c->host->run ? c->host->run(c->host->ctx, expr, nullptr, 0) : false;
    return true;
}

// --- the interpreter --------------------------------------------------------

static void fail(RpsResult *r, uint32_t line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, ap);
    va_end(ap);
    r->ok = false;
    r->line = line + 1;
}

bool rps_run(const char *text, const RpsHost *host, RpsResult *out) {
    Script s;
    index_lines(&s, text);

    Ctx c{};
    c.host = host;
    c.res = out;

    memset(out, 0, sizeof(*out));
    out->ok = true;

    Block stack[RPS_DEPTH];
    int depth = 0;

    char raw[RPS_LINE_MAX], line[RPS_LINE_MAX], w[32];

    for (uint32_t i = 0; i < s.lines; ) {
        if (host->poll && host->poll(host->ctx)) {
            fail(out, i, "stopped");
            return false;
        }

        get_line(&s, i, raw, sizeof(raw));
        const char *t = skip_ws(raw);
        if (!*t || *t == '#') { i++; continue; }

        // Substitution happens ONCE, before the statement is looked at, so a
        // variable can hold a whole argument list — and so `$` means the same
        // thing everywhere rather than only where the language remembered.
        rps_expand(&c.vars, t, line, sizeof(line));
        const char *rest = word(line, w, sizeof(w));
        out->executed++;

        if (!strcmp(w, "set")) {
            char name[RPS_NAME_MAX];
            rest = word(rest, name, sizeof(name));
            char val[RPS_VAL_MAX];
            snprintf(val, sizeof(val), "%s", skip_ws(rest));
            // A quoted value keeps its spaces; an unquoted one is taken whole,
            // which is what makes `set greeting hello there` do the obvious.
            if (val[0] == '"' || val[0] == '\'') word(skip_ws(rest), val, sizeof(val));
            if (!name[0]) { fail(out, i, "set needs a name"); return false; }
            if (!var_set(&c.vars, name, val)) { fail(out, i, "too many variables (%d)", RPS_VARS); return false; }
            i++;
            continue;
        }
        if (!strcmp(w, "inc") || !strcmp(w, "dec")) {
            char name[RPS_NAME_MAX], amt[16];
            rest = word(rest, name, sizeof(name));
            word(rest, amt, sizeof(amt));
            long step = 1;
            if (amt[0]) is_num(amt, &step);
            if (!strcmp(w, "dec")) step = -step;
            const char *cur = var_get(&c.vars, name, (uint32_t)strlen(name));
            long v = 0;
            if (cur) is_num(cur, &v);
            char nv[24]; snprintf(nv, sizeof(nv), "%ld", v + step);
            if (!var_set(&c.vars, name, nv)) { fail(out, i, "too many variables"); return false; }
            i++;
            continue;
        }
        if (!strcmp(w, "prompt")) {
            char name[RPS_NAME_MAX];
            rest = word(rest, name, sizeof(name));
            char buf[RPS_VAL_MAX] = {0};
            if (host->prompt) host->prompt(host->ctx, skip_ws(rest), buf, sizeof(buf));
            var_set(&c.vars, name, buf);
            i++;
            continue;
        }
        if (!strcmp(w, "capture")) {
            char name[RPS_NAME_MAX];
            rest = word(rest, name, sizeof(name));
            char buf[RPS_VAL_MAX] = {0};
            if (host->run) host->run(host->ctx, skip_ws(rest), buf, sizeof(buf));
            // Trailing newline removed: a captured value is almost always
            // compared against something, and a stray newline makes every
            // comparison fail for a reason nobody can see.
            uint32_t bl = (uint32_t)strlen(buf);
            while (bl && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r')) buf[--bl] = 0;
            var_set(&c.vars, name, buf);
            i++;
            continue;
        }
        if (!strcmp(w, "stop")) { out->ok = true; return true; }

        if (!strcmp(w, "if")) {
            if (depth >= RPS_DEPTH) { fail(out, i, "nested too deep (%d)", RPS_DEPTH); return false; }
            bool cond = false;
            if (!eval_cond(&c, skip_ws(rest), &cond)) { fail(out, i, "bad condition"); return false; }
            uint32_t end_line = 0, else_line = 0;
            if (!find_close(&s, i, &end_line, &else_line)) { fail(out, i, "if without end"); return false; }
            if (cond) {
                stack[depth].kind = BLK_IF;
                stack[depth].head = i;
                stack[depth].taken = true;
                depth++;
                i++;
            } else if (else_line) {
                // The else branch is the block now; its `end` closes it.
                stack[depth].kind = BLK_IF;
                stack[depth].head = i;
                stack[depth].taken = false;
                depth++;
                i = else_line + 1;
            } else {
                // Nothing to run. Jump PAST the end, not onto it — landing on
                // `end` executes it against a block that was never pushed.
                i = end_line + 1;
            }
            continue;
        }
        if (!strcmp(w, "else")) {
            if (!depth || stack[depth - 1].kind != BLK_IF) { fail(out, i, "else without if"); return false; }
            // Reaching `else` by running means the if branch was taken, so skip.
            uint32_t end_line = 0;
            if (!find_close(&s, stack[depth - 1].head, &end_line, nullptr)) { fail(out, i, "if without end"); return false; }
            depth--;
            // PAST the end, not onto it: the block is popped here, so letting
            // `end` execute would report it as closing nothing.
            i = end_line + 1;
            continue;
        }
        if (!strcmp(w, "while")) {
            if (depth >= RPS_DEPTH) { fail(out, i, "nested too deep (%d)", RPS_DEPTH); return false; }
            bool cond = false;
            if (!eval_cond(&c, skip_ws(rest), &cond)) { fail(out, i, "bad condition"); return false; }
            uint32_t end_line = 0;
            if (!find_close(&s, i, &end_line, nullptr)) { fail(out, i, "while without end"); return false; }
            if (!cond) { i = end_line + 1; continue; }
            stack[depth].kind = BLK_WHILE;
            stack[depth].head = i;
            depth++;
            i++;
            continue;
        }
        if (!strcmp(w, "end")) {
            if (!depth) { fail(out, i, "end without if or while"); return false; }
            if (stack[depth - 1].kind == BLK_WHILE) { i = stack[depth - 1].head; depth--; continue; }
            depth--;
            i++;
            continue;
        }
        if (!strcmp(w, "break") || !strcmp(w, "continue")) {
            // Unwind to the nearest while; an if between here and it is left.
            int d = depth;
            while (d > 0 && stack[d - 1].kind != BLK_WHILE) d--;
            if (!d) { fail(out, i, "%s outside a while", w); return false; }
            uint32_t end_line = 0;
            if (!find_close(&s, stack[d - 1].head, &end_line, nullptr)) { fail(out, i, "while without end"); return false; }
            if (!strcmp(w, "break")) {
                depth = d - 1;
                i = end_line + 1;
            } else {
                // Back to the `while` line, which re-tests and re-pushes its
                // own frame — so this one has to come off first, or the stack
                // grows by one on every continue.
                depth = d - 1;
                i = stack[d - 1].head;
            }
            continue;
        }

        // Anything else is a shell command.
        if (host->run) host->run(host->ctx, line, nullptr, 0);
        i++;
    }

    if (depth) { fail(out, s.lines ? s.lines - 1 : 0, "unclosed if or while"); return false; }
    return true;
}
