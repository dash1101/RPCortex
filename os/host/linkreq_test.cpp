// Link-mode request parsing: verbs, the rest-of-line field (spaces kept), and
// PUT's fields, including every way they can be malformed.
#include "linkreq.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }

int main(void) {
    LinkReq r;
    ck(link_parse("PING", &r) && !strcmp(r.verb, "PING") && !strcmp(r.rest, ""), "a bare verb");
    ck(link_parse("EXEC ls /home | wc -l", &r) && !strcmp(r.verb, "EXEC") &&
       !strcmp(r.rest, "ls /home | wc -l"), "EXEC keeps the whole command, spaces and all");
    ck(link_parse("LS /a dir/with  two", &r) && !strcmp(r.rest, "/a dir/with  two"),
       "a path keeps its spaces, double ones included");
    ck(!link_parse("", &r), "an empty line is not a request");
    ck(!link_parse(" PING", &r), "a leading space is not a verb");
    ck(!link_parse("TOOLONGVERB x", &r), "a verb past seven characters is refused");

    const char *sha = "9F0E1D2C3B4A59687766554433221100ffeeddccbbaa99887766554433221100";
    char line[256], hex[65]; uint32_t size = 0; const char *path = nullptr;
    snprintf(line, sizeof(line), "1436 %s /tmp/my file.app", sha);
    ck(link_parse_put(line, &size, hex, &path) && size == 1436 &&
       !strcmp(path, "/tmp/my file.app"), "PUT: size, digest and a path with a space");
    ck(!strcmp(hex, "9f0e1d2c3b4a59687766554433221100ffeeddccbbaa99887766554433221100"),
       "PUT: the digest comes back lower-case");
    snprintf(line, sizeof(line), "0 %s /empty", sha);
    ck(link_parse_put(line, &size, hex, &path) && size == 0, "PUT: an empty file is allowed");
    snprintf(line, sizeof(line), "4294967295 %s /big", sha);
    ck(link_parse_put(line, &size, hex, &path) && size == 4294967295u, "PUT: the largest 32-bit size");
    snprintf(line, sizeof(line), "4294967296 %s /big", sha);
    ck(!link_parse_put(line, &size, hex, &path), "PUT: a size past 32 bits is refused");
    snprintf(line, sizeof(line), "-1 %s /x", sha);
    ck(!link_parse_put(line, &size, hex, &path), "PUT: a negative size is refused");
    snprintf(line, sizeof(line), "12 %s", sha);
    ck(!link_parse_put(line, &size, hex, &path), "PUT: no path is refused");
    snprintf(line, sizeof(line), "12 %s ", sha);
    ck(!link_parse_put(line, &size, hex, &path), "PUT: an empty path is refused");
    ck(!link_parse_put("12 abc /x", &size, hex, &path), "PUT: a short digest is refused");
    snprintf(line, sizeof(line), "12 %.63sz /x", sha);
    ck(!link_parse_put(line, &size, hex, &path), "PUT: a non-hex digest is refused");
    ck(!link_parse_put("12x 00 /x", &size, hex, &path), "PUT: a size with junk is refused");

    printf("  linkreq: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
