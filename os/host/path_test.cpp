// Path resolution — the '.'/'..'/absolute/relative logic the fs commands rely on.
#include "path.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void eq(const char *cwd, const char *in, const char *want) {
    char out[128];
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
    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
