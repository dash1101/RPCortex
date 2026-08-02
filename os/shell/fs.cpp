// Filesystem commands — cd/pwd/ls/cat/mkdir/rm/mv/cp/touch/tree.
//
// The v1 sys_fs.py set, over littlefs. The one piece of state is a current
// working directory the shell carries; every command resolves its argument
// against it, so relative paths, '.', '..' and absolute paths all work the way
// they do in any shell.

#include "command.h"
#include "out.h"
#include "storage.h"
#include "path.h"
#include "fmt.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

static char g_cwd[128] = "/";

const char *fs_cwd(void) { return g_cwd; }

// Path resolution now lives in core/path.cpp (path_resolve), so it can be
// host-tested away from littlefs. This wrapper binds it to the shell's cwd.
static void resolve(const char *in, char *out, size_t cap) {
    path_resolve(g_cwd, in, out, cap);
}

static int cmd_pwd(int, char **) { out_multi("%s", g_cwd); return 0; }

static int cmd_cd(int argc, char **argv) {
    const char *arg = (argc >= 2) ? argv[1] : "/";
    char path[128];
    resolve(arg, path, sizeof(path));
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) { out_err("No such path: %s", path); return 1; }
    if (!is_dir) { out_err("Not a directory: %s", path); return 1; }
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    return 0;
}

// ls — v1's table: TYPE, SIZE, MODIFIED, NAME under a rule, directories first.
//
// The columns are the whole point. Someone who knows what a listing looks like
// on Vela should see the same shape here, so the widths, the separator row and
// the colours (cyan directories, yellow files, grey metadata) are v1's.
//
// Two walks rather than one buffered listing: the directory lives on flash and
// holding a whole listing in RAM to sort it is the habit this OS exists to
// avoid. Directories come first because that is the order v1 sorted into.
#define LS_DIR_COLOUR  "\033[96m"     // cyan   — v1's _CD
#define LS_FILE_COLOUR "\033[93m"     // yellow — v1's _CF
#define LS_META_COLOUR "\033[90m"     // grey   — v1's _CT

struct LsCtx { const char *base; bool dirs; uint32_t shown; };

static void ls_cb(void *ctx, const char *name, bool is_dir, uint32_t size) {
    LsCtx *c = (LsCtx *)ctx;
    if (c->dirs != is_dir) return;
    c->shown++;

    char full[192];
    if (strcmp(c->base, "/") == 0) snprintf(full, sizeof(full), "/%s", name);
    else                          snprintf(full, sizeof(full), "%s/%s", c->base, name);

    char size_s[12];
    if (is_dir) snprintf(size_s, sizeof(size_s), "-");
    else        fmt_size(size, size_s, sizeof(size_s));

    char when[24];
    fmt_time(storage_mtime(full), when, sizeof(when));

    const char *colour = is_dir ? LS_DIR_COLOUR : LS_FILE_COLOUR;
    out_multi("  %s%-5s%s  %s%-7s%s  %s%-19s%s  %s%s%s%s",
              colour, is_dir ? "DIR" : "FILE", C_RESET,
              LS_META_COLOUR, size_s, C_RESET,
              LS_META_COLOUR, when, C_RESET,
              colour, name, is_dir ? "/" : "", C_RESET);
}

static int cmd_ls(int argc, char **argv) {
    char path[128];
    resolve(argc >= 2 ? argv[1] : ".", path, sizeof(path));

    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) { out_err("Cannot list directory '%s'", path); return 1; }
    if (!is_dir) { out_err("Not a directory: %s", path); return 1; }

    out_multi("  %-5s  %-7s  %-19s  %s", "TYPE", "SIZE", "MODIFIED", "NAME");
    out_multi("  %s──────────────────────────────────────────────────────────%s",
              LS_META_COLOUR, C_RESET);

    LsCtx d{path, true, 0}, f{path, false, 0};
    if (!storage_walk(path, ls_cb, &d)) { out_err("Cannot list directory '%s'", path); return 1; }
    storage_walk(path, ls_cb, &f);
    if (d.shown + f.shown == 0) out_warn("Directory is empty.");
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: cat <file>"); return 1; }
    char path[128];
    resolve(argv[1], path, sizeof(path));
    AppSource src; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) { out_err("No such file: %s", path); return 1; }
    uint8_t chunk[128];
    uint32_t off = 0;
    while (off < src.size) {
        uint32_t want = src.size - off; if (want > sizeof(chunk)) want = sizeof(chunk);
        int n = src.read(src.ctx, off, chunk, want);
        if (n <= 0) break;
        out_write((const char *)chunk, (uint32_t)n);
        off += (uint32_t)n;
    }
    if (off) out_write("\n", 1);     // ensure the prompt starts on a fresh line
    storage_close_source(h);
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: mkdir <dir>"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    if (!storage_mkdir(path)) { out_err("Could not create %s", path); return 1; }
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: rm <path>"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    // A bare `rm` on a non-empty directory fails at the littlefs level rather
    // than recursively deleting — deleting a tree from a shell should be a
    // deliberate, separate act, not a silent side effect.
    if (!storage_remove(path)) { out_err("Could not remove %s  (directory not empty?)", path); return 1; }
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { out_multi("Usage: mv <from> <to>"); return 1; }
    char a[128], b[128]; resolve(argv[1], a, sizeof(a)); resolve(argv[2], b, sizeof(b));
    if (!storage_rename(a, b)) { out_err("Could not move %s", a); return 1; }
    return 0;
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) { out_multi("Usage: cp <from> <to>"); return 1; }
    char a[128], b[128]; resolve(argv[1], a, sizeof(a)); resolve(argv[2], b, sizeof(b));
    if (!storage_copy(a, b)) { out_err("Could not copy %s", a); return 1; }
    return 0;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: touch <file>"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    bool exists = storage_stat(path, nullptr, nullptr);
    if (exists) return 0;                          // already there; nothing to do
    if (!storage_write_file(path, (const uint8_t *)"", 0)) {
        out_err("Could not create %s", path); return 1;
    }
    return 0;
}

// tree — recursive listing, depth-capped at 8, the guard v1 used so a deep or
// cyclic-looking hierarchy cannot blow the stack. It recurses by re-walking each
// subdirectory with its full path, which the callback carries in its context.
struct TreeCtx { const char *base; int depth; };
static void tree_walk(const char *base, int depth);

static void tree_print(void *ctx, const char *name, bool is_dir, uint32_t size) {
    (void)size;
    TreeCtx *c = (TreeCtx *)ctx;
    for (int i = 0; i < c->depth; i++) out_write("  ", 2);
    out_multi("%s%s%s", is_dir ? C_BLUE : "", name, is_dir ? "/" C_RESET : "");
    if (is_dir && c->depth < 8) {
        char child[128];
        if (strcmp(c->base, "/") == 0) snprintf(child, sizeof(child), "/%s", name);
        else snprintf(child, sizeof(child), "%s/%s", c->base, name);
        tree_walk(child, c->depth + 1);
    }
}

static void tree_walk(const char *base, int depth) {
    TreeCtx c{base, depth};
    storage_walk(base, tree_print, &c);
}

static int cmd_tree(int argc, char **argv) {
    char path[128];
    resolve(argc >= 2 ? argv[1] : ".", path, sizeof(path));
    out_multi("%s%s%s", C_BLUE, path, C_RESET);
    tree_walk(path, 1);
    return 0;
}

// rename — mv restricted to the same directory, which is what people reach for
// when they only mean to change a name. `rename a.txt b.txt` in /logs must not
// quietly become `/b.txt` because the shell's cwd was somewhere else, so the
// destination is resolved against the SOURCE's directory, not the cwd.
static int cmd_rename(int argc, char **argv) {
    if (argc < 3) { out_multi("Usage: rename <old> <new>"); return 1; }
    if (strchr(argv[2], '/')) {
        out_err("A new name cannot contain '/'. Use mv to move a file.");
        return 1;
    }
    char from[128];
    resolve(argv[1], from, sizeof(from));
    char to[128];
    const char *slash = strrchr(from, '/');
    int dirlen = slash ? (int)(slash - from) : 0;
    snprintf(to, sizeof(to), "%.*s/%s", dirlen, from, argv[2]);
    if (!storage_rename(from, to)) { out_err("Could not rename %s", from); return 1; }
    return 0;
}

// df — the whole filesystem, v1's diskfree.
static int cmd_df(int, char **) {
    uint32_t total = storage_total_bytes();
    uint32_t free  = storage_free_bytes();
    uint32_t used  = total - free;
    out_multi("  Total : %u KB", (unsigned)(total / 1024));
    out_multi("  Used  : %u KB  (%u%%)", (unsigned)(used / 1024),
              (unsigned)(total ? used * 100 / total : 0));
    out_multi("  Free  : %u KB", (unsigned)(free / 1024));
    return 0;
}

// du — total bytes under a path, recursing with the same depth cap as tree.
struct DuCtx { const char *base; int depth; uint32_t bytes; uint32_t files; uint32_t dirs; };
static void du_walk(const char *base, int depth, DuCtx *acc);

static void du_cb(void *ctx, const char *name, bool is_dir, uint32_t size) {
    DuCtx *c = (DuCtx *)ctx;
    if (!is_dir) { c->bytes += size; c->files++; return; }
    c->dirs++;
    if (c->depth >= 8) return;
    char child[128];
    if (strcmp(c->base, "/") == 0) snprintf(child, sizeof(child), "/%s", name);
    else snprintf(child, sizeof(child), "%s/%s", c->base, name);
    du_walk(child, c->depth + 1, c);
}

// The accumulator is shared across the recursion; only base/depth change, and
// they are saved and restored so the parent's walk continues correctly.
static void du_walk(const char *base, int depth, DuCtx *acc) {
    const char *save_base = acc->base;
    int save_depth = acc->depth;
    acc->base = base; acc->depth = depth;
    storage_walk(base, du_cb, acc);
    acc->base = save_base; acc->depth = save_depth;
}

static int cmd_du(int argc, char **argv) {
    char path[128];
    resolve(argc >= 2 ? argv[1] : ".", path, sizeof(path));
    bool is_dir = false; uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size)) { out_err("No such path: %s", path); return 1; }
    if (!is_dir) {
        out_multi("  %-24s %6u B", path, (unsigned)size);
        return 0;
    }
    DuCtx c{path, 0, 0, 0, 0};
    du_walk(path, 0, &c);
    out_multi("  %s", path);
    out_multi("  %u B  in %u file%s, %u director%s",
              (unsigned)c.bytes, (unsigned)c.files, c.files == 1 ? "" : "s",
              (unsigned)c.dirs, c.dirs == 1 ? "y" : "ies");
    return 0;
}

void fs_register(void) {
    static const Command cmds[] = {
        {"pwd",    "print the working directory", cmd_pwd,    nullptr},
        {"cd",     "change directory",            cmd_cd,     nullptr},
        {"ls",     "list a directory",            cmd_ls,     nullptr},
        {"cat",    "print a file",                cmd_cat,    nullptr},
        {"mkdir",  "make a directory",            cmd_mkdir,  nullptr},
        {"rm",     "remove a file or empty dir",  cmd_rm,     nullptr},
        {"mv",     "move a file",                 cmd_mv,     nullptr},
        {"cp",     "copy a file",                 cmd_cp,     nullptr},
        {"rename", "rename in place",             cmd_rename, nullptr},
        {"touch",  "create an empty file",        cmd_touch,  nullptr},
        {"tree",   "recursive listing",           cmd_tree,   nullptr},
        {"df",     "filesystem usage",            cmd_df,     nullptr},
        {"du",     "size of a path",              cmd_du,     nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);

    // v1's second spellings. Same function, no extra registry slot.
    cmd_alias("ll",     "ls");
    cmd_alias("la",     "ls");
    cmd_alias("dir",    "ls");
    cmd_alias("chdir",  "cd");
    cmd_alias("read",   "cat");
    cmd_alias("open",   "cat");
    cmd_alias("view",   "cat");
    cmd_alias("more",   "cat");
    cmd_alias("less",   "cat");
    cmd_alias("del",    "rm");
    cmd_alias("delete", "rm");
    cmd_alias("rmdir",  "rm");
    cmd_alias("move",   "mv");
    cmd_alias("copy",   "cp");
    cmd_alias("ren",    "rename");
    cmd_alias("write",  "touch");
}
