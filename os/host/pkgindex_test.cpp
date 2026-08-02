// The package index string operations — dedup on add, exact-line removal, walk.
// The bugs that would let a package appear twice or resist removal live here.

#include "pkgindex.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }

static char buf[512];
static uint32_t len;
static void set(const char *s) { strcpy(buf, s); len = (uint32_t)strlen(s); }

struct Collect { char out[256]; };
static void collect(void *ctx, const char *name, const char *ver) {
    Collect *c = (Collect *)ctx;
    char line[64]; snprintf(line, sizeof(line), "%s=%s;", name, ver);
    strcat(c->out, line);
}

int main(void) {
    // add
    set("");
    len = pkgindex_add(buf, len, sizeof(buf), "novad1", "1.0");
    ck(strcmp(buf, "novad1,1.0\n") == 0, "add to an empty index");
    len = pkgindex_add(buf, len, sizeof(buf), "calc", "2.1");
    ck(strcmp(buf, "novad1,1.0\ncalc,2.1\n") == 0, "add a second package");

    // dedup
    uint32_t before = len;
    len = pkgindex_add(buf, len, sizeof(buf), "novad1", "9.9");
    ck(len == before, "adding an existing package is a no-op (kept one line)");
    ck(pkgindex_has(buf, len, "novad1"), "and it is still listed");

    // exact-name matching: a prefix must not count as present
    ck(!pkgindex_has(buf, len, "nova"), "'nova' is not 'novad1' (no prefix match)");
    ck(!pkgindex_has(buf, len, "calc2"), "'calc2' is not 'calc'");
    ck(pkgindex_has(buf, len, "calc"), "'calc' is present");

    // walk
    Collect c{{0}};
    pkgindex_walk(buf, len, collect, &c);
    ck(strcmp(c.out, "novad1=1.0;calc=2.1;") == 0, "walk yields each name and version");
    // walk must leave the buffer intact for reuse
    ck(strcmp(buf, "novad1,1.0\ncalc,2.1\n") == 0, "walk does not corrupt the buffer");

    // remove the middle/first exactly
    len = pkgindex_remove(buf, len, sizeof(buf), "novad1");
    ck(strcmp(buf, "calc,2.1\n") == 0, "remove takes out exactly the named line");
    ck(!pkgindex_has(buf, len, "novad1"), "and it is gone");
    ck(pkgindex_has(buf, len, "calc"), "the other survives");

    // remove a prefix-colliding name must NOT remove the real one
    set("d1,1.0\nd1x,2.0\n");
    len = pkgindex_remove(buf, len, sizeof(buf), "d1");
    ck(strcmp(buf, "d1x,2.0\n") == 0, "removing 'd1' leaves 'd1x' (no prefix removal)");

    // remove something absent is a no-op
    before = len;
    len = pkgindex_remove(buf, len, sizeof(buf), "ghost");
    ck(len == before && strcmp(buf, "d1x,2.0\n") == 0, "removing an absent package changes nothing");

    // remove the last remaining -> empty
    len = pkgindex_remove(buf, len, sizeof(buf), "d1x");
    ck(len == 0 && buf[0] == 0, "removing the last package empties the index");

    // a comment line is skipped by walk but preserved otherwise
    set("# repo index\nnovad1,1.0\n");
    Collect c2{{0}};
    pkgindex_walk(buf, len, collect, &c2);
    ck(strcmp(c2.out, "novad1=1.0;") == 0, "walk skips comment lines");

    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
