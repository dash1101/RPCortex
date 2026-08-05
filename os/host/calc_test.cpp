// The calc package's evaluator, on the host.
//
// v1's Calc handed the expression to Python's eval(), so the parser is the
// whole conversion — and a parser is nothing but edge cases. Finding them on a
// device one reboot at a time is not a plan, so calc.cpp keeps its arithmetic
// free of fw_* calls and this compiles that half directly.
#define CALC_HOST_TEST
#include "../apps/calc/calc.cpp"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int checks, fails;

static void num(const char *expr, double want) {
    checks++;
    double got = 0;
    char what[24];
    CalcErr e = calc_eval(expr, &got, what, sizeof(what));
    if (e != CALC_OK) {
        printf("  FAIL: %-24s error %d\n", expr, (int)e);
        fails++;
        return;
    }
    if (fabs(got - want) > 1e-9) {
        printf("  FAIL: %-24s got %.10g, want %.10g\n", expr, got, want);
        fails++;
    }
}

static void bad(const char *expr, CalcErr want) {
    checks++;
    double got = 0;
    char what[24];
    CalcErr e = calc_eval(expr, &got, what, sizeof(what));
    if (e != want) {
        printf("  FAIL: %-24s error %d, want %d\n", expr, (int)e, (int)want);
        fails++;
    }
}

static void fmt(double v, const char *want) {
    checks++;
    char buf[48];
    calc_format_num(v, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        printf("  FAIL: format %.10g -> '%s', want '%s'\n", v, buf, want);
        fails++;
    }
}

static void ibase(const char *in, const char *base, const char *want) {
    checks++;
    long long n = 0;
    if (!calc_parse_int(in, &n)) {
        printf("  FAIL: '%s' did not parse as an integer\n", in);
        fails++;
        return;
    }
    char buf[80];
    calc_format_int(n, base, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        printf("  FAIL: %s as %s -> '%s', want '%s'\n", in, base, buf, want);
        fails++;
    }
}

int main(void) {
    printf("calc_test - the calculator's evaluator\n");

    // Precedence and associativity: the reason a hand-written parser is worth
    // testing at all. Getting these wrong gives confidently wrong answers
    // rather than errors.
    num("1 + 2", 3);
    num("2 + 3 * 4", 14);              // not 20
    num("(2 + 3) * 4", 20);
    num("10 - 2 - 3", 5);              // left-associative, not 11
    num("100 / 5 / 2", 10);
    num("2 ^ 3 ^ 2", 512);             // right-associative, not 64
    num("-2 ^ 2", -4);                 // unary binds looser than ^
    num("2 * -3", -6);
    num("--5", 5);
    num("7 % 3", 1);

    // The example from v1's own usage text.
    num("3 * (4 + 2) / 1.5", 12);

    // Numbers.
    num("1.5", 1.5);
    num(".5", 0.5);
    num("2e3", 2000);
    num("1.5e-2", 0.015);
    num("  42  ", 42);

    // Functions and constants — what `from math import *` put in scope in v1.
    num("sqrt(16)", 4);
    num("sqrt(2)", 1.41421356237309504);
    num("abs(-7)", 7);
    num("floor(2.7)", 2);
    num("floor(-2.1)", -3);
    num("ceil(2.1)", 3);
    num("ceil(-2.7)", -2);
    num("pi", 3.14159265358979323846);
    num("sqrt(4) + 1", 3);

    // Errors, each distinguishable so the message can be specific.
    bad("1 / 0", CALC_DIV_ZERO);
    bad("5 % 0", CALC_DIV_ZERO);
    bad("nope(2)", CALC_UNKNOWN_NAME);
    bad("banana", CALC_UNKNOWN_NAME);
    bad("sqrt(-1)", CALC_DOMAIN);
    bad("1 +", CALC_SYNTAX);
    bad("(1 + 2", CALC_SYNTAX);
    bad("1 2", CALC_SYNTAX);           // trailing junk is not success
    bad("", CALC_SYNTAX);

    // The name is reported, so the error can name it.
    {
        checks++;
        double v; char what[24];
        calc_eval("wibble + 1", &v, what, sizeof(what));
        if (strcmp(what, "wibble") != 0) {
            printf("  FAIL: unknown name reported as '%s'\n", what);
            fails++;
        }
    }

    // Formatting, matching v1's _fmt: a whole number loses its fraction.
    fmt(4.0, "4");
    fmt(-4.0, "-4");
    fmt(0.0, "0");
    fmt(1.5, "1.5");
    fmt(-1.25, "-1.25");
    fmt(12.0, "12");

    // Base conversion, matching v1's int(value, 0).
    ibase("255", "hex", "0xFF");
    ibase("0xff", "dec", "255");
    ibase("0b1010", "dec", "10");
    ibase("0o17", "dec", "15");
    ibase("10", "bin", "0b1010");
    ibase("8", "oct", "0o10");
    ibase("-255", "hex", "-0xFF");
    ibase("0", "hex", "0x0");

    checks++;
    long long dummy;
    if (calc_parse_int("12x", &dummy)) {
        printf("  FAIL: '12x' parsed as an integer\n");
        fails++;
    }

        // Numbers past what a double can hold exactly.
    //
    // `calc 2^99` printed 9223372036854775807.9999999999 -- LLONG_MAX with a
    // fraction bolted on, because the value was cast to a long long first and
    // that is undefined. It looked precise enough to be believed, which is the
    // worst way for a calculator to be wrong.
    {
        char b[48];
        auto has = [&](double v, const char *needle) {
            checks++;
            calc_format_num(v, b, sizeof(b));
            if (!strstr(b, needle)) {
                printf("  FAIL: format %.10g -> '%s', wanted '%s' in it\n", v, b, needle);
                fails++;
            }
        };
        auto lacks = [&](double v, const char *needle) {
            checks++;
            calc_format_num(v, b, sizeof(b));
            if (strstr(b, needle)) {
                printf("  FAIL: format %.10g -> '%s', should not contain '%s'\n", v, b, needle);
                fails++;
            }
        };

        has(1e30, "e30");
        lacks(1e30, "9223372036854775807");
        has(-1e30, "-");
        // Small numbers are NOT captured by this: 0.000000001 is perfectly
        // readable and turning it into an exponent would help nobody.
        lacks(1e-9, "e-");

        // The ordinary range must not be captured by this path: these were
        // already printing correctly.
        fmt(4, "4");
        fmt(1.5, "1.5");
        lacks(999999999999.0, "e");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
