// Filesystem commands — cd/pwd/ls/cat/mkdir/rm/mv/cp/touch/tree.
//
// The v1 sys_fs.py set, over littlefs. The one piece of state is a current
// working directory the shell carries; every command resolves its argument
// against it, so relative paths, '.', '..' and absolute paths all work the way
// they do in any shell.

#include "command.h"
#include "storage.h"
#include "path.h"

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

static int cmd_pwd(int, char **) { printf("%s\n", g_cwd); return 0; }

static int cmd_cd(int argc, char **argv) {
    const char *arg = (argc >= 2) ? argv[1] : "/";
    char path[128];
    resolve(arg, path, sizeof(path));
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) { printf("no such path: %s\n", path); return 1; }
    if (!is_dir) { printf("not a directory: %s\n", path); return 1; }
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = 0;
    return 0;
}

// ls prints directories first (each with a trailing '/'), then files with sizes.
// Two walks rather than buffering the listing — the directory is on flash and
// the panel-era habit of not holding a whole listing in RAM still applies.
struct LsCtx { bool dirs; };
static void ls_cb(void *ctx, const char *name, bool is_dir, uint32_t size) {
    LsCtx *c = (LsCtx *)ctx;
    if (c->dirs && is_dir) printf("  %s/\n", name);
    else if (!c->dirs && !is_dir) printf("  %-24s %6u B\n", name, (unsigned)size);
}
static int cmd_ls(int argc, char **argv) {
    char path[128];
    resolve(argc >= 2 ? argv[1] : ".", path, sizeof(path));
    LsCtx d{true}, f{false};
    if (!storage_walk(path, ls_cb, &d)) { printf("cannot list %s\n", path); return 1; }
    storage_walk(path, ls_cb, &f);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { printf("usage: cat <file>\n"); return 1; }
    char path[128];
    resolve(argv[1], path, sizeof(path));
    AppSource src; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) { printf("no such file: %s\n", path); return 1; }
    uint8_t chunk[128];
    uint32_t off = 0;
    while (off < src.size) {
        uint32_t want = src.size - off; if (want > sizeof(chunk)) want = sizeof(chunk);
        int n = src.read(src.ctx, off, chunk, want);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) putchar(chunk[i]);
        off += (uint32_t)n;
    }
    if (off) putchar('\n');          // ensure the prompt starts on a fresh line
    storage_close_source(h);
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { printf("usage: mkdir <dir>\n"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    if (!storage_mkdir(path)) { printf("could not create %s\n", path); return 1; }
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: rm <path>\n"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    // A bare `rm` on a non-empty directory fails at the littlefs level rather
    // than recursively deleting — deleting a tree from a shell should be a
    // deliberate, separate act, not a silent side effect.
    if (!storage_remove(path)) { printf("could not remove %s (non-empty dir?)\n", path); return 1; }
    return 0;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { printf("usage: mv <from> <to>\n"); return 1; }
    char a[128], b[128]; resolve(argv[1], a, sizeof(a)); resolve(argv[2], b, sizeof(b));
    if (!storage_rename(a, b)) { printf("could not move\n"); return 1; }
    return 0;
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) { printf("usage: cp <from> <to>\n"); return 1; }
    char a[128], b[128]; resolve(argv[1], a, sizeof(a)); resolve(argv[2], b, sizeof(b));
    if (!storage_copy(a, b)) { printf("could not copy\n"); return 1; }
    return 0;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) { printf("usage: touch <file>\n"); return 1; }
    char path[128]; resolve(argv[1], path, sizeof(path));
    bool exists = storage_stat(path, nullptr, nullptr);
    if (exists) return 0;                          // already there; nothing to do
    if (!storage_write_file(path, (const uint8_t *)"", 0)) {
        printf("could not create %s\n", path); return 1;
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
    for (int i = 0; i < c->depth; i++) printf("  ");
    printf("%s%s\n", name, is_dir ? "/" : "");
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
    printf("%s\n", path);
    tree_walk(path, 1);
    return 0;
}

void fs_register(void) {
    static const Command cmds[] = {
        {"pwd",   "print the working directory", cmd_pwd,   nullptr},
        {"cd",    "change directory",            cmd_cd,    nullptr},
        {"ls",    "list a directory",            cmd_ls,    nullptr},
        {"cat",   "print a file",                cmd_cat,   nullptr},
        {"mkdir", "make a directory",            cmd_mkdir, nullptr},
        {"rm",    "remove a file or empty dir",  cmd_rm,    nullptr},
        {"mv",    "move / rename",               cmd_mv,    nullptr},
        {"cp",    "copy a file",                 cmd_cp,    nullptr},
        {"touch", "create an empty file",        cmd_touch, nullptr},
        {"tree",  "recursive listing",           cmd_tree,  nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
