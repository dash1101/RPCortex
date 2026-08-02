// Text-processing commands — echo, grep, wc, head, tail, find.
//
// The v1 sys_text.py set. Line logic (counting, iteration) is in
// core/textcore.cpp and host-tested; these commands read a file into a bounded
// buffer and hand it there. A file over the cap is processed up to the cap with
// a note, the way v1 warned on large sort inputs — the alternative is holding an
// unbounded file in RAM to count its lines.

#include "command.h"
#include "storage.h"
#include "textcore.h"
#include "path.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEXT_CAP 16384

const char *fs_cwd(void);       // the shell's working directory (fs.cpp)

// Read a file (resolved against cwd) into a fresh buffer. Caller frees. Returns
// nullptr if the file is missing; sets *truncated if it was longer than the cap.
static char *read_text(const char *arg, uint32_t *out_len, bool *truncated) {
    char path[128];
    path_resolve(fs_cwd(), arg, path, sizeof(path));
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr) || is_dir) return nullptr;
    char *buf = (char *)malloc(TEXT_CAP + 1);
    if (!buf) return nullptr;
    uint32_t n = storage_read_file(path, (uint8_t *)buf, TEXT_CAP);
    buf[n] = 0;
    if (out_len) *out_len = n;
    if (truncated) *truncated = (n == TEXT_CAP);
    return buf;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    printf("\n");
    return 0;
}

struct GrepCtx { const char *pat; int hits; };
static void grep_line(void *ctx, const char *line, uint32_t n) {
    GrepCtx *g = (GrepCtx *)ctx;
    if (strstr(line, g->pat)) { printf("%u: %s\n", (unsigned)n, line); g->hits++; }
}
static int cmd_grep(int argc, char **argv) {
    if (argc < 3) { printf("usage: grep <pattern> <file>\n"); return 1; }
    uint32_t len = 0; bool trunc = false;
    char *buf = read_text(argv[2], &len, &trunc);
    if (!buf) { printf("no such file: %s\n", argv[2]); return 1; }
    GrepCtx g{argv[1], 0};
    text_for_lines(buf, len, grep_line, &g);
    if (trunc) printf("(file truncated at %u KB)\n", TEXT_CAP / 1024);
    free(buf);
    return g.hits ? 0 : 1;       // grep's convention: no match is non-zero
}

static int cmd_wc(int argc, char **argv) {
    if (argc < 2) { printf("usage: wc <file>\n"); return 1; }
    uint32_t len = 0; bool trunc = false;
    char *buf = read_text(argv[1], &len, &trunc);
    if (!buf) { printf("no such file: %s\n", argv[1]); return 1; }
    uint32_t l, w, b;
    text_count(buf, len, &l, &w, &b);
    printf("  %u lines  %u words  %u bytes%s\n",
           (unsigned)l, (unsigned)w, (unsigned)b, trunc ? "  (truncated)" : "");
    free(buf);
    return 0;
}

struct HeadCtx { uint32_t limit, shown; };
static void head_line(void *ctx, const char *line, uint32_t n) {
    HeadCtx *h = (HeadCtx *)ctx;
    if (n <= h->limit) { printf("%s\n", line); h->shown++; }
}
static int cmd_head(int argc, char **argv) {
    if (argc < 2) { printf("usage: head <file> [n]\n"); return 1; }
    uint32_t n = (argc >= 3) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 10;
    uint32_t len = 0;
    char *buf = read_text(argv[1], &len, nullptr);
    if (!buf) { printf("no such file: %s\n", argv[1]); return 1; }
    HeadCtx h{n, 0};
    text_for_lines(buf, len, head_line, &h);
    free(buf);
    return 0;
}

struct TailCtx { uint32_t total, keep, seen; };
static void tail_line(void *ctx, const char *line, uint32_t idx) {
    TailCtx *t = (TailCtx *)ctx;
    if (idx > t->total - t->keep) printf("%s\n", line);
}
static int cmd_tail(int argc, char **argv) {
    if (argc < 2) { printf("usage: tail <file> [n]\n"); return 1; }
    uint32_t n = (argc >= 3) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 10;
    uint32_t len = 0;
    char *buf = read_text(argv[1], &len, nullptr);
    if (!buf) { printf("no such file: %s\n", argv[1]); return 1; }
    uint32_t total = text_line_count(buf, len);
    TailCtx t{total, n < total ? n : total, 0};
    text_for_lines(buf, len, tail_line, &t);
    free(buf);
    return 0;
}

// find — recursive filename search, depth-capped like tree.
struct FindCtx { const char *needle; const char *base; int depth; };
static void find_walk(const char *base, const char *needle, int depth);
static void find_cb(void *ctx, const char *name, bool is_dir, uint32_t) {
    FindCtx *f = (FindCtx *)ctx;
    char full[128];
    if (strcmp(f->base, "/") == 0) snprintf(full, sizeof(full), "/%s", name);
    else snprintf(full, sizeof(full), "%s/%s", f->base, name);
    if (strstr(name, f->needle)) printf("  %s%s\n", full, is_dir ? "/" : "");
    if (is_dir && f->depth < 8) find_walk(full, f->needle, f->depth + 1);
}
static void find_walk(const char *base, const char *needle, int depth) {
    FindCtx f{needle, base, depth};
    storage_walk(base, find_cb, &f);
}
static int cmd_find(int argc, char **argv) {
    if (argc < 2) { printf("usage: find <name> [path]\n"); return 1; }
    char base[128];
    path_resolve(fs_cwd(), argc >= 3 ? argv[2] : ".", base, sizeof(base));
    find_walk(base, argv[1], 0);
    return 0;
}

void text_register(void) {
    static const Command cmds[] = {
        {"echo", "print arguments",        cmd_echo, nullptr},
        {"grep", "grep <pattern> <file>",  cmd_grep, nullptr},
        {"wc",   "count lines/words/bytes", cmd_wc,  nullptr},
        {"head", "first lines of a file",  cmd_head, nullptr},
        {"tail", "last lines of a file",   cmd_tail, nullptr},
        {"find", "find <name> [path]",     cmd_find, nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
