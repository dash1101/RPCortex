// Path resolution — the '.'/'..'/absolute/relative logic the fs commands rely on.
#include "path.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void eq(const char *cwd, const char *in, const char *want) {
    char out[128];
    // (the bool is checked by the long-path cases below)
    path_resolve(cwd, in, out, sizeof(out));
    checks++;
    if (strcmp(out, want) != 0) {
        fails++;
        printf("  FAIL: resolve(%s, %s) = %s, want %s\n", cwd, in, out, want);
    }
}

int main(void) {
    eq("/", "x", "/x");
    eq("/", "/x", "/x");
    eq("/a/b", "c", "/a/b/c");
    eq("/a/b", "/c", "/c");
    eq("/a/b", "..", "/a");
    eq("/a/b", "../..", "/");
    eq("/a/b", "../../..", "/");            // cannot escape above root
    eq("/a/b", ".", "/a/b");
    eq("/a/b", "./c", "/a/b/c");
    eq("/a", "b/../c", "/a/c");
    eq("/a/b/c", "../../x/./y", "/a/x/y");
    eq("/", "..", "/");
    eq("/", ".", "/");
    eq("/a", "", "/a");                     // empty relative stays put
    eq("/", "a//b", "/a/b");                // collapse double slash
    eq("/a/b", "/", "/");
    // Long names are kept whole, never dropped. A 40-character name used to
    // vanish, so "/tmp/<long>" came back as "/tmp" and touch made a FILE there.
    {
        char longname[61]; memset(longname, 'y', 60); longname[60] = 0;
        char want[80]; snprintf(want, sizeof(want), "/tmp/%s", longname);
        eq("/", want, want);
        char rel[80]; snprintf(rel, sizeof(rel), "/home/root/%s", longname);
        eq("/home/root", longname, rel);
    }
    // And a result that does not fit fails, with nothing in out, rather than
    // being cut down to some shorter path that exists.
    {
        char big[200]; memset(big, 'z', 199); big[199] = 0;
        char in[220]; snprintf(in, sizeof(in), "/home/%s", big);
        char out[128] = "junk";
        bool ok = path_resolve("/", in, out, sizeof(out));
        checks++;
        if (ok || out[0]) { fails++; printf("  FAIL: an over-long path was accepted as '%s'\n", out); }

        char small[8] = "junk";
        ok = path_resolve("/", "/abcdefgh", small, sizeof(small));
        checks++;
        if (ok || small[0]) { fails++; printf("  FAIL: a path one too long for out was accepted\n"); }
        ok = path_resolve("/", "/abcdef", small, sizeof(small));
        checks++;
        if (!ok || strcmp(small, "/abcdef")) { fails++; printf("  FAIL: a path that exactly fits was refused\n"); }
    }
    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
