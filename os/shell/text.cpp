// Text-processing commands — echo, grep, wc, head, tail, find.
//
// The v1 sys_text.py set. Line logic (counting, iteration) is in
// core/textcore.cpp and host-tested; these commands read a file into a bounded
// buffer and hand it there. A file over the cap is processed up to the cap with
// a note, the way v1 warned on large sort inputs — the alternative is holding an
// unbounded file in RAM to count its lines.

#include "command.h"
#include "out.h"
#include "storage.h"
#include "textcore.h"
#include "path.h"
#include "interrupt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TEXT_CAP 16384

const char *fs_cwd(void);       // the shell's working directory (fs.cpp)
const char *shell_stdin(void);  // this stage's piped input, or nullptr (shell.cpp)
uint32_t    shell_stdin_len(void);

// A copy of the piped input, so every caller can free() its buffer the same way
// whether the bytes came from a file or a pipe.
static char *dup_stdin(uint32_t *out_len) {
    const char *in = shell_stdin();
    if (!in) return nullptr;
    uint32_t n = shell_stdin_len();
    char *buf = (char *)malloc(n + 1);
    if (!buf) return nullptr;
    memcpy(buf, in, n);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

// Read a file (resolved against cwd) into a fresh buffer. Caller frees. Returns
// nullptr if the file is missing; sets *truncated if it was longer than the cap.
//
// Piped input always wins over a file argument. Inside a pipeline the remaining
// arguments are options, not filenames — `cat log | head 5` means five lines,
// and reading a file called "5" instead is the bug this rule exists to prevent.
// The cost is that `cat a | grep x b` ignores b, which no one types.
static char *read_text(const char *arg, uint32_t *out_len, bool *truncated) {
    if (truncated) *truncated = false;
    if (shell_stdin() || !arg) return dup_stdin(out_len);
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
    // Through the data channel, not printf: `echo hi > f` has to put "hi" in the
    // file, and a raw printf would send it to the console and leave f empty.
    for (int i = 1; i < argc; i++) {
        out_write(argv[i], (uint32_t)strlen(argv[i]));
        if (i + 1 < argc) out_write(" ", 1);
    }
    out_write("\n", 1);
    return 0;
}

struct GrepCtx { const char *pat; int hits; };
static void grep_line(void *ctx, const char *line, uint32_t n) {
    GrepCtx *g = (GrepCtx *)ctx;
    if (strstr(line, g->pat)) { out_multi("%s%u:%s %s", C_GRAY, (unsigned)n, C_RESET, line); g->hits++; }
}
static int cmd_grep(int argc, char **argv) {
    if (argc < 3 && !shell_stdin()) { out_multi("Usage: grep <pattern> <file>"); return 1; }
    uint32_t len = 0; bool trunc = false;
    char *buf = read_text(argc > 2 ? argv[2] : nullptr, &len, &trunc);
    if (!buf) { out_err("No such file: %s", argc > 2 ? argv[2] : "(stdin)"); return 1; }
    GrepCtx g{argv[1], 0};
    text_for_lines(buf, len, grep_line, &g);
    if (trunc) out_warn("File truncated at %u KB.", TEXT_CAP / 1024);
    free(buf);
    return g.hits ? 0 : 1;       // grep's convention: no match is non-zero
}

static int cmd_wc(int argc, char **argv) {
    if (argc < 2 && !shell_stdin()) { out_multi("Usage: wc <file>"); return 1; }
    uint32_t len = 0; bool trunc = false;
    char *buf = read_text(argc > 1 ? argv[1] : nullptr, &len, &trunc);
    if (!buf) { out_err("No such file: %s", argc > 1 ? argv[1] : "(stdin)"); return 1; }
    uint32_t l, w, b;
    text_count(buf, len, &l, &w, &b);
    out_multi("  %u lines  %u words  %u bytes%s",
              (unsigned)l, (unsigned)w, (unsigned)b, trunc ? "  (truncated)" : "");
    free(buf);
    return 0;
}

// The line count for head/tail. Reading from a file it is argv[2]; reading a
// pipe there is no file argument, so `head 5` means five lines.
static uint32_t line_limit(int argc, char **argv) {
    int idx = shell_stdin() ? 1 : 2;
    if (argc > idx) {
        char *end = nullptr;
        unsigned long v = strtoul(argv[idx], &end, 10);
        if (end != argv[idx] && *end == 0 && v > 0) return (uint32_t)v;
    }
    return 10;
}

struct HeadCtx { uint32_t limit, shown; };
static void head_line(void *ctx, const char *line, uint32_t n) {
    HeadCtx *h = (HeadCtx *)ctx;
    if (n <= h->limit) { out_multi("%s", line); h->shown++; }
}
static int cmd_head(int argc, char **argv) {
    if (argc < 2 && !shell_stdin()) { out_multi("Usage: head <file> [n]"); return 1; }
    uint32_t n = line_limit(argc, argv);
    uint32_t len = 0;
    char *buf = read_text(argc > 1 ? argv[1] : nullptr, &len, nullptr);
    if (!buf) { out_err("No such file: %s", argc > 1 ? argv[1] : "(stdin)"); return 1; }
    HeadCtx h{n, 0};
    text_for_lines(buf, len, head_line, &h);
    free(buf);
    return 0;
}

struct TailCtx { uint32_t total, keep, seen; };
static void tail_line(void *ctx, const char *line, uint32_t idx) {
    TailCtx *t = (TailCtx *)ctx;
    if (idx > t->total - t->keep) out_multi("%s", line);
}
static int cmd_tail(int argc, char **argv) {
    if (argc < 2 && !shell_stdin()) { out_multi("Usage: tail <file> [n]"); return 1; }
    uint32_t n = line_limit(argc, argv);
    uint32_t len = 0;
    char *buf = read_text(argc > 1 ? argv[1] : nullptr, &len, nullptr);
    if (!buf) { out_err("No such file: %s", argc > 1 ? argv[1] : "(stdin)"); return 1; }
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
    if (strstr(name, f->needle)) out_multi("  %s%s", full, is_dir ? "/" : "");
    if (is_dir && f->depth < 8) find_walk(full, f->needle, f->depth + 1);
}
static void find_walk(const char *base, const char *needle, int depth) {
    FindCtx f{needle, base, depth};
    storage_walk(base, find_cb, &f);
}
static int cmd_find(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: find <name> [path]"); return 1; }
    char base[128];
    path_resolve(fs_cwd(), argc >= 3 ? argv[2] : ".", base, sizeof(base));
    find_walk(base, argv[1], 0);
    return 0;
}

// sort — alphabetical, in place over an index of line pointers.
//
// The lines are not copied; the buffer is split on '\n' and an array of pointers
// into it is sorted. That is one allocation of 4 bytes per line instead of a
// second copy of the file, which is the difference between sorting a 16 KB file
// and failing to. Insertion sort: n is bounded by SORT_MAX and a quicksort's
// recursion is a worse trade on a 4 KB stack than n²/4 comparisons on 2000 items.
#define SORT_MAX 2000

static int cmd_sort(int argc, char **argv) {
    if (argc < 2 && !shell_stdin()) { out_multi("Usage: sort <file>"); return 1; }
    uint32_t len = 0; bool trunc = false;
    char *buf = read_text(argc > 1 ? argv[1] : nullptr, &len, &trunc);
    if (!buf) { out_err("No such file: %s", argc > 1 ? argv[1] : "(stdin)"); return 1; }

    char **lines = (char **)malloc(SORT_MAX * sizeof(char *));
    if (!lines) { free(buf); out_err("Not enough memory to sort."); return 1; }

    uint32_t n = 0;
    char *p = buf;
    while (p < buf + len && n < SORT_MAX) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    // Strip a trailing '\r' so a CRLF file sorts on its real content.
    for (uint32_t i = 0; i < n; i++) {
        size_t l = strlen(lines[i]);
        if (l && lines[i][l - 1] == '\r') lines[i][l - 1] = 0;
    }

    for (uint32_t i = 1; i < n; i++) {
        char *key = lines[i];
        uint32_t j = i;
        while (j > 0 && strcmp(lines[j - 1], key) > 0) { lines[j] = lines[j - 1]; j--; }
        lines[j] = key;
    }

    for (uint32_t i = 0; i < n && !intr_check(); i++) out_multi("%s", lines[i]);
    if (trunc)        out_warn("File truncated at %u KB.", TEXT_CAP / 1024);
    if (n == SORT_MAX) out_warn("Stopped at %u lines.", (unsigned)SORT_MAX);
    free(lines);
    free(buf);
    return 0;
}

// uniq — drop CONSECUTIVE duplicates, which is what uniq means everywhere and
// what v1 did. It needs no buffer of its own: only the previous line.
struct UniqCtx { char prev[128]; bool has_prev; uint32_t dropped; };
static void uniq_line(void *ctx, const char *line, uint32_t) {
    UniqCtx *u = (UniqCtx *)ctx;
    if (u->has_prev && strncmp(u->prev, line, sizeof(u->prev) - 1) == 0) { u->dropped++; return; }
    out_multi("%s", line);
    strncpy(u->prev, line, sizeof(u->prev) - 1);
    u->prev[sizeof(u->prev) - 1] = 0;
    u->has_prev = true;
}
static int cmd_uniq(int argc, char **argv) {
    if (argc < 2 && !shell_stdin()) { out_multi("Usage: uniq <file>"); return 1; }
    uint32_t len = 0;
    char *buf = read_text(argc > 1 ? argv[1] : nullptr, &len, nullptr);
    if (!buf) { out_err("No such file: %s", argc > 1 ? argv[1] : "(stdin)"); return 1; }
    UniqCtx u; u.prev[0] = 0; u.has_prev = false; u.dropped = 0;
    text_for_lines(buf, len, uniq_line, &u);
    free(buf);
    return 0;
}

// hex — the classic 16-byte dump: offset, hex, printable. Streams the file so a
// hexdump of something large does not need it in RAM.
static int cmd_hex(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: hex <file> [n]"); return 1; }
    uint32_t want = (argc >= 3) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 256;
    char path[128];
    path_resolve(fs_cwd(), argv[1], path, sizeof(path));
    AppSource src; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) { out_err("No such file: %s", path); return 1; }
    if (want > src.size) want = src.size;

    uint8_t row[16];
    for (uint32_t off = 0; off < want && !intr_check(); off += 16) {
        uint32_t n = want - off; if (n > 16) n = 16;
        int got = src.read(src.ctx, off, row, n);
        if (got <= 0) break;
        char hexpart[52] = {0}, text[20] = {0};
        int hp = 0;
        for (int i = 0; i < 16; i++) {
            if (i < got) hp += snprintf(hexpart + hp, sizeof(hexpart) - hp, "%02x ", row[i]);
            else         hp += snprintf(hexpart + hp, sizeof(hexpart) - hp, "   ");
            if (i == 7)  hp += snprintf(hexpart + hp, sizeof(hexpart) - hp, " ");
        }
        for (int i = 0; i < got; i++)
            text[i] = (row[i] >= 32 && row[i] < 127) ? (char)row[i] : '.';
        out_multi("%s%08lx%s  %s |%s|", C_GRAY, (unsigned long)off, C_RESET, hexpart, text);
    }
    storage_close_source(h);
    return 0;
}

static int cmd_basename(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: basename <path>"); return 1; }
    const char *s = strrchr(argv[1], '/');
    out_multi("%s", (s && s[1]) ? s + 1 : (s ? "/" : argv[1]));
    return 0;
}

static int cmd_dirname(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: dirname <path>"); return 1; }
    const char *s = strrchr(argv[1], '/');
    if (!s)        { out_multi("."); return 0; }
    if (s == argv[1]) { out_multi("/"); return 0; }
    out_multi("%.*s", (int)(s - argv[1]), argv[1]);
    return 0;
}

void text_register(void) {
    static const Command cmds[] = {
        {"echo",     "print arguments",           cmd_echo,     nullptr},
        {"grep",     "grep <pattern> <file>",     cmd_grep,     nullptr},
        {"wc",       "count lines/words/bytes",   cmd_wc,       nullptr},
        {"head",     "first lines of a file",     cmd_head,     nullptr},
        {"tail",     "last lines of a file",      cmd_tail,     nullptr},
        {"find",     "find <name> [path]",        cmd_find,     nullptr},
        {"sort",     "sort a file's lines",       cmd_sort,     nullptr},
        {"uniq",     "drop repeated lines",       cmd_uniq,     nullptr},
        {"hex",      "hex <file> [n]",            cmd_hex,      nullptr},
        {"basename", "file name part of a path",  cmd_basename, nullptr},
        {"dirname",  "directory part of a path",  cmd_dirname,  nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);

    cmd_alias("print",   "echo");
    cmd_alias("count",   "wc");
    cmd_alias("hexdump", "hex");
}
