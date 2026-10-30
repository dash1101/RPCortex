// The repo index scanner, run against the REAL index.json from the package repo.
//
// A hand-written fixture only ever contains what the test author remembered to
// put in it, and the interesting facts about this file are the ones nobody would
// think to invent — a description three times longer than the buffer meant to
// hold it, for instance, which turned an entry invisible before this test
// existed. The real file is checked in as a fixture so that a format change
// becomes a failing test rather than a device that finds no packages.
#include "repoindex.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0, fails = 0;
static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

struct Count { int n; char first[REPO_NAME_MAX]; bool saw_long_desc; };
static bool count_cb(void *ctx, const RepoEntry *e) {
    Count *c = (Count *)ctx;
    if (c->n == 0) snprintf(c->first, sizeof(c->first), "%s", e->name);
    c->n++;
    if (strlen(e->desc) >= REPO_DESC_MAX - 1) c->saw_long_desc = true;
    return true;
}

static char *slurp(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return nullptr; }
    b[n] = 0; fclose(f);
    *len = (uint32_t)n;
    return b;
}

int main(void) {
    printf("  repoindex\n");

    // --- versions -----------------------------------------------------------
    ok(repo_version_cmp("1.0.0", "1.0.0") == 0,  "version: equal");
    ok(repo_version_cmp("1.2.0", "1.10.0") < 0,  "version: 1.10 is newer than 1.2");
    ok(repo_version_cmp("2.0.0", "1.99.9") > 0,  "version: major wins");
    ok(repo_version_cmp("2.1",   "2.1.0") == 0,  "version: missing component is zero");
    ok(repo_version_cmp("3.0.0", "2.1.1") > 0,   "version: RPCMark 3.0.0 beats 2.1.1");
    ok(repo_version_cmp("0.9.1", "1.0.0") < 0,   "version: 0.9.1 is older than 1.0.0");

    // A version is never newer than itself. The old comparison returned "newer"
    // the moment it met a suffix on the left without looking at the right, so a
    // package at 1.0-beta was offered an upgrade to 1.0-beta, for ever.
    const char *odd[] = {"1.0-beta", "v1.0", "2.1rc1", "1.0b", "a.b", "-1", "0x10",
                         "1.2.3-4", "", "1..2", "99999999999", "1.0.0.463"};
    for (const char *v : odd)
        ok(repo_version_cmp(v, v) == 0, "version: a suffixed version equals itself");
    ok(repo_version_cmp("1.0-beta", "1.0-beta") == 0, "version: 1.0-beta equals 1.0-beta");
    ok(repo_version_cmp("1.0b", "1.0") > 0,      "version: a suffix sorts newer, as before");
    ok(repo_version_cmp("1.0", "1.0b") < 0,      "version: and the other way round");
    ok(repo_version_cmp("1.0rc1", "1.0rc2") < 0, "version: two suffixes compare as text");
    ok(repo_version_cmp("1.0rc2", "1.0rc1") > 0, "version: and the other way round");
    ok(repo_version_cmp("1.0rc", "1.0rc1") < 0,  "version: a longer suffix with the same start is newer");
    ok(repo_version_cmp("1.1rc1", "1.0") > 0,    "version: the number still decides before any suffix");
    ok(repo_version_cmp("1.0", "1.1rc1") < 0,    "version: and the other way round");
    ok(repo_version_cmp("2.0.0.463", "2.0.0.462") > 0, "version: the build number is a fourth component");
    // vMAJOR.ITER with the build as the third number: "2.11.1203".
    ok(repo_version_cmp("2.0.486", "2.0.485") > 0,      "version: a later build of one iteration is newer");
    ok(repo_version_cmp("2.0.486", "2.0.0.483") > 0,   "version: the new form is newer than any build of the old one");
    ok(repo_version_cmp("2.0.0.483", "2.0.486") < 0,   "version: and an old manifest never beats a new device");
    ok(repo_version_cmp("2.11.1203", "2.9.2000") > 0,  "version: the iteration compares as a number, not as text");
    ok(repo_version_cmp("2.10.5", "2.9.99") > 0,       "version: v2.10 follows v2.9");
    ok(repo_version_cmp("3.0.1", "2.99.9999") > 0,     "version: a new generation beats every iteration of the last");
    ok(repo_version_cmp("2.0.486", "2.0.486") == 0,    "version: the same build is the same");
    // The minor is written with two digits: "2.00" is "2.0", and "2.10" follows "2.09".
    ok(repo_version_cmp("2.00.486", "2.0.486") == 0,   "version: a zero-padded minor is the same number");
    ok(repo_version_cmp("2.00.486", "2.0.0.483") > 0,  "version: the padded form is newer than the old four-part one");
    ok(repo_version_cmp("2.10.1", "2.09.999") > 0,     "version: v2.10 follows v2.09");
    ok(repo_version_cmp("2.01.5", "2.00.999") > 0,     "version: the first patch beats every build of the release");
    ok(repo_version_cmp("99999999999999999999", "1") > 0, "version: a huge number saturates instead of wrapping");
    ok(repo_version_cmp("1", "99999999999999999999") < 0, "version: and the other way round");
    // 2^32 + 1: wrapped to 32 bits it would read as 1 and sort below 2.
    ok(repo_version_cmp("4294967297", "2") > 0,  "version: a number past 32 bits does not wrap to a small one");
    ok(repo_version_cmp("2", "4294967297") < 0,  "version: and the other way round");

    // --- synthetic shapes ---------------------------------------------------
    {
        const char *j = "{\"name\":\"repo\",\"maintainer\":\"someone\",\"packages\":["
                        "{\"name\":\"Alpha\",\"ver\":\"1.0.0\",\"desc\":\"first\","
                        "\"author\":\"a\",\"url\":\"https://h/a.pkg\"},"
                        "{\"name\":\"Beta\",\"ver\":\"2.0.0\",\"desc\":\"second\","
                        "\"author\":\"b\",\"url\":\"https://h/b.pkg\"}]}";
        Count c{};
        uint32_t n = repo_walk(j, strlen(j), count_cb, &c);
        ok(n == 2 && c.n == 2, "two entries");
        // The top-level "name" must not be mistaken for a package.
        ok(!strcmp(c.first, "Alpha"), "the repo's own name is not an entry");

        RepoEntry e;
        ok(repo_find(j, strlen(j), "beta", &e), "find is case-insensitive");
        ok(!strcmp(e.ver, "2.0.0") && !strcmp(e.url, "https://h/b.pkg"), "found fields");
        ok(!repo_find(j, strlen(j), "gamma", &e), "absent name is not found");
    }
    {
        // Pretty-printed, reordered keys, an unknown extra field, and a
        // non-string value — all of which a real index may grow.
        const char *j =
            "{\n  \"packages\": [\n    {\n"
            "      \"url\"  : \"https://h/x.pkg\",\n"
            "      \"size\" : 12345,\n"
            "      \"ver\"  : \"1.2.3\",\n"
            "      \"name\" : \"Xray\",\n"
            "      \"extra\": {\"nested\": \"ignored\"}\n"
            "    }\n  ]\n}\n";
        RepoEntry e;
        ok(repo_find(j, strlen(j), "Xray", &e), "whitespace and key order do not matter");
        ok(!strcmp(e.ver, "1.2.3"), "version read past a numeric field");
        ok(!strcmp(e.url, "https://h/x.pkg"), "url read past a nested object");
    }
    {
        // An over-long URL must drop the entry, not truncate it — a shortened
        // URL is a request to somewhere real and wrong.
        char j[600];
        char big[REPO_URL_MAX + 60];
        memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
        snprintf(j, sizeof(j), "{\"packages\":[{\"name\":\"Big\",\"url\":\"https://h/%s\"}]}", big);
        RepoEntry e;
        ok(!repo_find(j, strlen(j), "Big", &e), "an over-long URL drops the entry");
    }
    {
        const char *j = "{\"packages\":[{\"ver\":\"1.0\",\"url\":\"https://h/x\"}]}";
        RepoEntry e;
        Count c{};
        ok(repo_walk(j, strlen(j), count_cb, &c) == 0, "an entry with no name is skipped");
        (void)e;
    }
    {
        const char *j = "{\"packages\":[{\"name\":\"NoUrl\",\"ver\":\"1.0\"}]}";
        Count c{};
        ok(repo_walk(j, strlen(j), count_cb, &c) == 0, "an entry with no URL is skipped");
    }
    {
        ok(repo_walk("", 0, nullptr, nullptr) == 0,           "empty input");
        ok(repo_walk("not json", 8, nullptr, nullptr) == 0,   "garbage input");
        const char *trunc = "{\"packages\":[{\"name\":\"A\",\"url\":\"htt";
        ok(repo_walk(trunc, strlen(trunc), nullptr, nullptr) == 0, "truncated input");
    }

    // --- the real index -----------------------------------------------------
    {
        uint32_t len = 0;
        char *j = slurp("fixtures/index.json", &len);
        if (!j) {
            printf("    SKIP real index (fixtures/index.json missing)\n");
        } else {
            Count c{};
            uint32_t n = repo_walk(j, len, count_cb, &c);
            printf("       real index: %u entries, %u bytes\n", n, len);
            ok(n == 20, "real index: all 20 packages parsed");

            // The finding that motivated truncating descriptions: one entry's
            // blurb is over three times the buffer. Refusing it would have made
            // that package invisible with no error anywhere.
            ok(c.saw_long_desc, "real index: a description longer than the buffer is kept");

            RepoEntry e;
            ok(repo_find(j, len, "RPCMark", &e), "real index: RPCMark present");
            ok(!strcmp(e.ver, "3.0.0"), "real index: RPCMark is at 3.0.0");
            ok(strstr(e.url, "rpcmark.pkg") != nullptr, "real index: RPCMark url");
            ok(repo_version_cmp(e.ver, "2.1.1") > 0, "real index: 3.0.0 offers an upgrade over 2.1.1");

            // Every entry must be installable: a name and a URL that survived.
            struct V { bool all; } v{true};
            repo_walk(j, len, [](void *ctx, const RepoEntry *x) {
                V *vv = (V *)ctx;
                if (!x->name[0] || strncmp(x->url, "https://", 8) != 0) vv->all = false;
                return true;
            }, &v);
            ok(v.all, "real index: every entry has a name and an https URL");
            free(j);
        }
    }

    // --- the v2 index, which is what this OS actually installs from ---------
    {
        uint32_t len = 0;
        char *j = slurp("fixtures/index-v2.json", &len);
        if (!j) {
            printf("    SKIP v2 index (fixtures/index-v2.json missing)\n");
        } else {
            RepoEntry e;
            ok(repo_find(j, len, "greet", &e), "v2 index: greet present");
            ok(!strcmp(e.ver, "1.0"), "v2 index: version");
            ok(!strcmp(e.arch, "armv6m"), "v2 index: arch parsed");
            ok(!strcmp(e.abi, "1.2"), "v2 index: abi parsed");
            ok(e.size == 1732, "v2 index: numeric size parsed past the string fields");
            ok(strlen(e.sha256) == 64, "v2 index: a full 64-hex-digit hash survived");
            ok(!strcmp(e.sha256,
                       "44959f9fa4d7293c9f3a0ee8e4328c4923038c01e4c14a3d4353d8b3c7dcbc04"),
               "v2 index: the hash is exact");
            ok(strstr(e.url, "greet.app") != nullptr, "v2 index: url points at an .app");
            free(j);
        }
    }
    {
        // A hash too long to hold must drop the entry. Keeping a truncated one
        // would compare unequal against every download and make the package
        // permanently uninstallable with nothing explaining why.
        char j[400];
        snprintf(j, sizeof(j),
                 "{\"packages\":[{\"name\":\"H\",\"url\":\"https://h/x.app\",\"sha256\":\"%s\"}]}",
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefTOOLONG");
        RepoEntry e;
        ok(!repo_find(j, strlen(j), "H", &e), "an over-long hash drops the entry");
    }
    {
        // The v1 index has no hash/abi/arch at all; those entries must still
        // parse, just without the extra guarantees.
        const char *j = "{\"packages\":[{\"name\":\"Old\",\"ver\":\"1.0\",\"url\":\"https://h/o.pkg\"}]}";
        RepoEntry e;
        ok(repo_find(j, strlen(j), "Old", &e), "v1-format entry still parses");
        ok(e.sha256[0] == 0 && e.size == 0, "v1-format entry reports no hash and no size");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
