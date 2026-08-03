// Calc — the shell calculator, converted from v1's Calc package.
//
// v1 evaluated expressions by handing the string to Python's eval(). There is
// no interpreter here, so the parser below IS the conversion: recursive descent
// over the same grammar people were already typing, with the same operators and
// the same math functions in scope.
//
// Everything above `--- the package ---` is deliberately free of fw_* calls and
// of anything that needs a device, so calc_eval and calc_convert compile and
// run on the host. That is where the arithmetic is actually tested; an
// expression parser is all edge cases, and finding them on hardware one reboot
// at a time is not a plan.
#include "rpc_app.h"

// --- the evaluator ----------------------------------------------------------
//
// Grammar, loosest binding first:
//
//   expr   := term  (('+' | '-') term)*
//   term   := unary (('*' | '/' | '%') unary)*
//   unary  := ('-' | '+')? power
//   power  := atom ('^' unary)?              right-associative, as ^ should be
//   atom   := number | name '(' expr ')' | name | '(' expr ')'

enum CalcErr {
    CALC_OK = 0,
    CALC_SYNTAX,
    CALC_UNKNOWN_NAME,
    CALC_DIV_ZERO,
    CALC_DOMAIN,          // sqrt(-1), log(0)
};

struct CalcCtx {
    const char *p;
    CalcErr     err;
    char        what[24];      // the name that was not recognised, for the message
};

static double parse_expr(CalcCtx *c);

static void skip_ws(CalcCtx *c) { while (*c->p == ' ' || *c->p == '\t') c->p++; }

static bool name_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
           (ch >= '0' && ch <= '9');
}

// strtod without the locale machinery, and without pulling in a dependency the
// app would have to carry. Handles leading digits, a fraction, and an exponent.
static double parse_number(CalcCtx *c) {
    double v = 0.0;
    while (*c->p >= '0' && *c->p <= '9') v = v * 10.0 + (*c->p++ - '0');
    if (*c->p == '.') {
        c->p++;
        double scale = 0.1;
        while (*c->p >= '0' && *c->p <= '9') { v += (*c->p++ - '0') * scale; scale *= 0.1; }
    }
    if (*c->p == 'e' || *c->p == 'E') {
        const char *save = c->p;
        c->p++;
        int sign = 1;
        if (*c->p == '-') { sign = -1; c->p++; }
        else if (*c->p == '+') c->p++;
        if (*c->p < '0' || *c->p > '9') { c->p = save; return v; }   // "2e" is not an exponent
        int ex = 0;
        while (*c->p >= '0' && *c->p <= '9') ex = ex * 10 + (*c->p++ - '0');
        for (int i = 0; i < ex; i++) v = sign > 0 ? v * 10.0 : v / 10.0;
    }
    return v;
}

static double d_abs(double v)  { return v < 0 ? -v : v; }
static double d_floor(double v) {
    double t = (double)(long long)v;
    return (v < 0 && t != v) ? t - 1 : t;
}
static double d_ceil(double v) {
    double t = (double)(long long)v;
    return (v > 0 && t != v) ? t + 1 : t;
}

// Newton's method. The app is built without linking libm, and pulling it in for
// one function would cost more than the eight iterations below.
static double d_sqrt(double v) {
    if (v < 0) return -1.0;
    if (v == 0) return 0.0;
    double x = v;
    for (int i = 0; i < 24; i++) x = 0.5 * (x + v / x);
    return x;
}

static double call_fn(CalcCtx *c, const char *name, double a) {
    struct Fn { const char *n; double (*f)(double); };
    static const Fn fns[] = {
        { "sqrt", d_sqrt }, { "abs", d_abs }, { "floor", d_floor }, { "ceil", d_ceil },
    };
    for (unsigned i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        const char *n = fns[i].n;
        const char *m = name;
        while (*n && *n == *m) { n++; m++; }
        if (*n == 0 && *m == 0) {
            double r = fns[i].f(a);
            if (fns[i].f == d_sqrt && a < 0) { c->err = CALC_DOMAIN; return 0; }
            return r;
        }
    }
    c->err = CALC_UNKNOWN_NAME;
    unsigned k = 0;
    while (name[k] && k < sizeof(c->what) - 1) { c->what[k] = name[k]; k++; }
    c->what[k] = 0;
    return 0;
}

static double parse_atom(CalcCtx *c) {
    skip_ws(c);

    if (*c->p == '(') {
        c->p++;
        double v = parse_expr(c);
        skip_ws(c);
        if (*c->p == ')') c->p++;
        else c->err = CALC_SYNTAX;
        return v;
    }

    if ((*c->p >= '0' && *c->p <= '9') || *c->p == '.') return parse_number(c);

    if (name_char(*c->p) && !(*c->p >= '0' && *c->p <= '9')) {
        char name[24];
        unsigned n = 0;
        while (name_char(*c->p) && n < sizeof(name) - 1) name[n++] = *c->p++;
        name[n] = 0;

        skip_ws(c);
        if (*c->p == '(') {
            c->p++;
            double a = parse_expr(c);
            skip_ws(c);
            if (*c->p == ')') c->p++;
            else c->err = CALC_SYNTAX;
            return call_fn(c, name, a);
        }

        // A bare name: the two constants v1's `from math import *` put in scope.
        if (name[0] == 'p' && name[1] == 'i' && name[2] == 0) return 3.14159265358979323846;
        if (name[0] == 'e' && name[1] == 0)                   return 2.71828182845904523536;

        c->err = CALC_UNKNOWN_NAME;
        unsigned k = 0;
        while (name[k] && k < sizeof(c->what) - 1) { c->what[k] = name[k]; k++; }
        c->what[k] = 0;
        return 0;
    }

    c->err = CALC_SYNTAX;
    return 0;
}

static double parse_unary(CalcCtx *c);

static double parse_power(CalcCtx *c) {
    double base = parse_atom(c);
    skip_ws(c);
    if (*c->p == '^') {
        c->p++;
        double ex = parse_unary(c);          // right-associative
        // Integer exponents only: repeated multiplication, no libm.
        long long n = (long long)ex;
        if ((double)n != ex) { c->err = CALC_DOMAIN; return 0; }
        double r = 1.0;
        bool neg = n < 0;
        if (neg) n = -n;
        for (long long i = 0; i < n; i++) r *= base;
        if (neg) {
            if (r == 0) { c->err = CALC_DIV_ZERO; return 0; }
            r = 1.0 / r;
        }
        return r;
    }
    return base;
}

static double parse_unary(CalcCtx *c) {
    skip_ws(c);
    if (*c->p == '-') { c->p++; return -parse_unary(c); }
    if (*c->p == '+') { c->p++; return  parse_unary(c); }
    return parse_power(c);
}

static double parse_term(CalcCtx *c) {
    double v = parse_unary(c);
    while (true) {
        skip_ws(c);
        char op = *c->p;
        if (op != '*' && op != '/' && op != '%') return v;
        c->p++;
        double r = parse_unary(c);
        if (c->err) return 0;
        if (op == '*') v *= r;
        else {
            if (r == 0) { c->err = CALC_DIV_ZERO; return 0; }
            if (op == '/') v /= r;
            else           v = (double)((long long)v % (long long)r);
        }
    }
}

static double parse_expr(CalcCtx *c) {
    double v = parse_term(c);
    while (true) {
        skip_ws(c);
        char op = *c->p;
        if (op != '+' && op != '-') return v;
        c->p++;
        double r = parse_term(c);
        if (c->err) return 0;
        v = (op == '+') ? v + r : v - r;
    }
}

// Evaluate `src`. Returns the error, with the result through `out` and the
// offending name (if any) through `what`.
CalcErr calc_eval(const char *src, double *out, char *what, unsigned what_cap) {
    CalcCtx c;
    c.p = src;
    c.err = CALC_OK;
    c.what[0] = 0;

    double v = parse_expr(&c);
    skip_ws(&c);
    if (!c.err && *c.p) c.err = CALC_SYNTAX;      // trailing junk is not success

    if (out) *out = v;
    if (what && what_cap) {
        unsigned k = 0;
        while (c.what[k] && k < what_cap - 1) { what[k] = c.what[k]; k++; }
        what[k] = 0;
    }
    return c.err;
}

// Parse an integer in any base (0x / 0b / 0o / decimal), the way v1's int(v, 0)
// did. Returns false when the whole string is not one.
bool calc_parse_int(const char *s, long long *out) {
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    else if (*s == '+') s++;

    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
    else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }

    if (!*s) return false;
    long long v = 0;
    for (; *s; s++) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else if (*s == ' ' || *s == '\t') break;
        else return false;
        if (d >= base) return false;
        v = v * base + d;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return false;

    *out = neg ? -v : v;
    return true;
}

// Render `n` in `base` ("hex" / "bin" / "oct" / "dec") into `out`.
void calc_format_int(long long n, const char *base, char *out, unsigned cap) {
    const char *prefix = "";
    int radix = 10;
    if      (base[0] == 'h') { prefix = "0x"; radix = 16; }
    else if (base[0] == 'b') { prefix = "0b"; radix = 2;  }
    else if (base[0] == 'o') { prefix = "0o"; radix = 8;  }

    char digits[72];
    unsigned n_digits = 0;
    unsigned long long u = (n < 0) ? (unsigned long long)(-n) : (unsigned long long)n;
    if (u == 0) digits[n_digits++] = '0';
    while (u) {
        int d = (int)(u % (unsigned)radix);
        digits[n_digits++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        u /= (unsigned)radix;
    }

    unsigned k = 0;
    if (n < 0 && k < cap - 1) out[k++] = '-';
    for (const char *q = prefix; *q && k < cap - 1; q++) out[k++] = *q;
    while (n_digits && k < cap - 1) out[k++] = digits[--n_digits];
    out[k] = 0;
}

// Format a result the way v1's _fmt did: a whole number prints without a
// fractional part, everything else keeps ten significant digits.
void calc_format_num(double v, char *out, unsigned cap) {
    long long whole = (long long)v;
    if ((double)whole == v && d_abs(v) < 1e15) {
        unsigned k = 0;
        char digits[24];
        unsigned n = 0;
        unsigned long long u = whole < 0 ? (unsigned long long)(-whole)
                                         : (unsigned long long)whole;
        if (u == 0) digits[n++] = '0';
        while (u) { digits[n++] = (char)('0' + (u % 10)); u /= 10; }
        if (whole < 0 && k < cap - 1) out[k++] = '-';
        while (n && k < cap - 1) out[k++] = digits[--n];
        out[k] = 0;
        return;
    }

    // Not whole: integer part, then ten fractional digits with the trailing
    // zeros trimmed, which is what "%.10g" was doing for the common cases.
    bool neg = v < 0;
    if (neg) v = -v;
    long long ip = (long long)v;
    double fp = v - (double)ip;

    char ipbuf[24];
    unsigned n = 0;
    unsigned long long u = (unsigned long long)ip;
    if (u == 0) ipbuf[n++] = '0';
    while (u) { ipbuf[n++] = (char)('0' + (u % 10)); u /= 10; }

    unsigned k = 0;
    if (neg && k < cap - 1) out[k++] = '-';
    while (n && k < cap - 1) out[k++] = ipbuf[--n];

    char frac[12];
    unsigned fn = 0;
    for (int i = 0; i < 10; i++) {
        fp *= 10.0;
        int d = (int)fp;
        if (d > 9) d = 9;
        frac[fn++] = (char)('0' + d);
        fp -= d;
    }
    while (fn && frac[fn - 1] == '0') fn--;
    if (fn && k < cap - 1) {
        out[k++] = '.';
        for (unsigned i = 0; i < fn && k < cap - 1; i++) out[k++] = frac[i];
    }
    out[k] = 0;
}

// --- the package ------------------------------------------------------------
#ifndef CALC_HOST_TEST

RPC_APP("calc");

static bool streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void usage(void) {
    fw_printf("Usage:\n");
    fw_printf("  calc <expression>      e.g. calc 3 * (4 + 2) / 1.5\n");
    fw_printf("  calc hex <value>       to hexadecimal\n");
    fw_printf("  calc bin <value>       to binary\n");
    fw_printf("  calc oct <value>       to octal\n");
    fw_printf("  calc dec <value>       to decimal (accepts 0x / 0b / 0o)\n");
    fw_printf("  Functions: sqrt abs floor ceil      Constants: pi e\n");
}

static int calc_cmd(int argc, char **argv) {
    if (argc < 2 || streq(argv[1], "help") || streq(argv[1], "-h") ||
        streq(argv[1], "--help") || streq(argv[1], "?")) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    // Base conversion, when the first word names one.
    if (streq(argv[1], "hex") || streq(argv[1], "bin") ||
        streq(argv[1], "oct") || streq(argv[1], "dec")) {
        if (argc < 3) { fw_printf("Usage: calc %s <value>\n", argv[1]); return 1; }
        long long n;
        if (!calc_parse_int(argv[2], &n)) {
            fw_printf("Not an integer: '%s'\n", argv[2]);
            return 1;
        }
        char buf[80];
        calc_format_int(n, argv[1], buf, sizeof(buf));
        fw_printf("%s\n", buf);
        return 0;
    }

    // Otherwise join the arguments back into one expression. The shell has
    // already split on spaces, and `calc 3 * (4 + 2)` is how people type it.
    char expr[160];
    unsigned k = 0;
    for (int i = 1; i < argc && k < sizeof(expr) - 1; i++) {
        if (i > 1 && k < sizeof(expr) - 1) expr[k++] = ' ';
        for (const char *q = argv[i]; *q && k < sizeof(expr) - 1; q++) expr[k++] = *q;
    }
    expr[k] = 0;

    double v;
    char what[24];
    CalcErr e = calc_eval(expr, &v, what, sizeof(what));
    switch (e) {
        case CALC_OK: break;
        case CALC_DIV_ZERO:
            fw_printf("Division by zero.\n");
            return 1;
        case CALC_UNKNOWN_NAME:
            fw_printf("Unknown name: '%s'\n", what);
            fw_printf("  Available: sqrt abs floor ceil, and pi / e.\n");
            return 1;
        case CALC_DOMAIN:
            fw_printf("Outside what that operation accepts.\n");
            return 1;
        default:
            fw_printf("Bad expression: %s\n", expr);
            return 1;
    }

    char buf[48];
    calc_format_num(v, buf, sizeof(buf));
    fw_printf("%s\n", buf);
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("calc", "shell calculator, and base conversion", calc_cmd);
    return 0;
}

#endif  // CALC_HOST_TEST
