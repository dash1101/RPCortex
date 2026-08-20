// backup_test — the snapshot package against a filesystem in memory.
//
// Everything this package can get wrong is quiet. A name that escapes its
// directory writes somewhere nobody asked about; a join that truncates names a
// DIFFERENT file, and on a restore that means overwriting the wrong one; a
// restore that trusts the name of whatever it finds in a snapshot directory
// will copy a stray file into /os on the strength of it. None of those raise
// anything, here or on a board — the command prints a cheerful line either way.
//
// So the package's source is compiled in with a fake filesystem underneath it,
// and the interesting cases are the ones where it must REFUSE.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <vector>

#include "../include/rpc_app.h"

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

// --- a filesystem in a map --------------------------------------------------

struct FakeNode {
    bool        is_dir;
    std::string data;
};
static std::map<std::string, FakeNode> g_fs;

static void fs_reset(void) { g_fs.clear(); }
static void fs_file(const char *path, const std::string &data) {
    g_fs[path] = FakeNode{false, data};
}
static void fs_dir(const char *path) { g_fs[path] = FakeNode{true, ""}; }
static bool fs_has(const char *path) { return g_fs.count(path) != 0; }
static std::string fs_read(const char *path) {
    auto it = g_fs.find(path);
    return it == g_fs.end() ? std::string("<absent>") : it->second.data;
}

// Immediate children of `dir`, in a stable order so an index means the same
// thing twice running — which is what fw_dir_entry promises.
static std::vector<std::string> fs_children(const char *dir) {
    std::string prefix = dir;
    if (prefix.empty() || prefix.back() != '/') prefix += '/';
    std::vector<std::string> out;
    for (auto &kv : g_fs) {
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = kv.first.substr(prefix.size());
        if (rest.empty() || rest.find('/') != std::string::npos) continue;
        out.push_back(rest);
    }
    return out;
}

// --- the ABI, enough of it to link ------------------------------------------

static char     g_out[16384];
static unsigned g_out_len;
static void     out_reset(void) { g_out_len = 0; g_out[0] = 0; }
static bool     out_has(const char *needle) { return strstr(g_out, needle) != nullptr; }

// What the heap will hand out, so the "no block big enough" path is reachable.
static unsigned g_largest = 1u << 20;

// Wall clock. year 0 stands for a device that has never seen a time server.
static FwTime g_now;

// The command the package registered, so the test drives the real entry point
// rather than a copy of its argument handling.
static RpcCommandFn g_cmd;

extern "C" {
int fw_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + g_out_len, sizeof(g_out) - g_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_out_len += (unsigned)n;
    return n;
}
void *fw_malloc(size_t n)  { return malloc(n ? n : 1); }
void  fw_free(void *p)     { free(p); }
uint32_t fw_heap_largest(void) { return g_largest; }
uint32_t fw_millis(void)       { return 1234567; }

int fw_time_get(FwTime *out) {
    if (!out) return 0;
    *out = g_now;
    return g_now.year ? 1 : 0;
}

int fw_file_exists(const char *path) { return fs_has(path) ? 1 : 0; }

uint32_t fw_file_size(const char *path) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || it->second.is_dir) return 0;
    return (uint32_t)it->second.data.size();
}

uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || it->second.is_dir) return 0;
    uint32_t n = (uint32_t)it->second.data.size();
    if (n > cap) n = cap;
    memcpy(buf, it->second.data.data(), n);
    return n;
}

int fw_file_write(const char *path, const void *data, uint32_t len) {
    g_fs[path] = FakeNode{false, std::string((const char *)data, len)};
    return 1;
}

// When set, the fake filesystem is "full": a streamed copy writes as much as it
// can and then fails, exactly as storage_copy does when a write hits a full
// volume partway — leaving a TRUNCATED destination behind. That partial file is
// the whole reason bk_copy has to clean up after a failed copy.
static bool g_fs_full = false;

// The streamed copy the package now uses instead of read-then-write. Whole file
// on success, nothing held in between; a missing source is a failure, not an
// empty copy.
int fw_file_copy(const char *from, const char *to) {
    auto it = g_fs.find(from);
    if (it == g_fs.end() || it->second.is_dir) return 0;
    const std::string &src = it->second.data;
    if (g_fs_full) {
        g_fs[to] = FakeNode{false, src.substr(0, src.size() / 2)};   // the half that fit
        return 0;
    }
    g_fs[to] = FakeNode{false, src};
    return 1;
}

// A directory only goes when it is empty — the same rule storage_remove has,
// and the reason `backup remove` empties one first.
int fw_file_remove(const char *path) {
    auto it = g_fs.find(path);
    if (it == g_fs.end()) return 0;
    if (it->second.is_dir && !fs_children(path).empty()) return 0;
    g_fs.erase(it);
    return 1;
}

int fw_mkdir(const char *path) {
    if (fs_has(path)) return 0;              // as littlefs does: EXIST is a failure
    fs_dir(path);
    return 1;
}

int fw_dir_count(const char *path) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || !it->second.is_dir) return -1;
    return (int)fs_children(path).size();
}

int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out) {
    auto it = g_fs.find(path);
    if (it == g_fs.end() || !it->second.is_dir) return -1;
    auto kids = fs_children(path);
    if (index >= kids.size()) return 0;
    const std::string &name = kids[index];
    snprintf(out->name, sizeof(out->name), "%s", name.c_str());
    std::string full = std::string(path) + "/" + name;
    out->is_dir = g_fs[full].is_dir ? 1 : 0;
    out->size   = (uint32_t)g_fs[full].data.size();
    return 1;
}

int rpc_register_command(const char *, const char *, RpcCommandFn fn) {
    g_cmd = fn;
    return 1;
}
}  // extern "C"

// The package itself. Its helpers are static, which is exactly why the source
// is included rather than linked.
#include "../apps/backup/backup.cpp"

// --- driving the command ----------------------------------------------------

static int run(const char *a0 = nullptr, const char *a1 = nullptr,
               const char *a2 = nullptr, const char *a3 = nullptr) {
    out_reset();
    char  buf[4][64];
    char *argv[5];
    int   argc = 1;
    argv[0] = (char *)"backup";
    const char *in[4] = {a0, a1, a2, a3};
    for (int i = 0; i < 4 && in[i]; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%s", in[i]);
        argv[argc++] = buf[i];
    }
    return g_cmd(argc, argv);
}

// A device with a full set of config files on it.
static void fs_populate(void) {
    fs_reset();
    fs_dir("/os");
    fs_dir("/etc");
    fs_file("/os/registry.cfg", "Settings.Theme=dark\nWiFi.Net0_SSID=home\n");
    fs_file("/os/users.cfg",    "dash:hash:admin\n");
    fs_file("/etc/startup.cfg", "wifi connect\n");
    // tasks.cfg and services.cfg deliberately absent: the common case is a
    // device that has some of these and not others.
}

int main(void) {
    printf("backup_test - names, joins, and the refusals\n");

    app_main(0);
    ck(g_cmd != nullptr, "the package registers its command");
    if (!g_cmd) return 1;

    // --- names --------------------------------------------------------------
    {
        ck(bk_name_ok("nightly"), "a plain name is fine");
        ck(bk_name_ok("2026-08-17_1"), "digits, dash and underscore are fine");
        ck(bk_name_ok("before.wifi"), "an interior dot is fine");
        ck(!bk_name_ok(""), "an empty name is refused");
        ck(!bk_name_ok(nullptr), "a null name is refused");
        ck(!bk_name_ok(".."), "'..' is refused");
        ck(!bk_name_ok("."), "'.' is refused");
        ck(!bk_name_ok(".hidden"), "a leading dot is refused");
        ck(!bk_name_ok("../../os"), "an escaping name is refused");
        ck(!bk_name_ok("a/b"), "a slash is refused");
        ck(!bk_name_ok("with space"), "a space is refused");
        char longname[BK_NAME_MAX + 4];
        memset(longname, 'a', sizeof(longname));
        longname[sizeof(longname) - 1] = 0;
        ck(!bk_name_ok(longname), "a name past the limit is refused");
    }

    // --- joining ------------------------------------------------------------
    {
        char p[32];
        ck(bk_join(p, sizeof(p), "/etc/backups", "nightly"), "a join fits");
        ck(strcmp(p, "/etc/backups/nightly") == 0, "and reads as expected");
        // The important ones: refusing beats truncating, because a truncated
        // path names a different file and a restore would overwrite it. Both
        // halves have to be able to run out of room — a check that only ever
        // trips in the directory loop says nothing about the name loop, which
        // is the half a snapshot name is pasted into.
        char small[12];
        ck(!bk_join(small, sizeof(small), "/etc/backups", "nightly"),
           "a join whose DIRECTORY does not fit is refused");
        char medium[16];
        ck(!bk_join(medium, sizeof(medium), "/etc", "averylongname"),
           "a join whose NAME does not fit is refused, not truncated");
        char exact[11];
        ck(bk_join(exact, sizeof(exact), "/etc", "abcde"), "a join that exactly fits is kept");
        ck(strcmp(exact, "/etc/abcde") == 0, "and is the whole path");
    }

    // --- leaf names ---------------------------------------------------------
    {
        ck(strcmp(bk_leaf("/os/registry.cfg"), "registry.cfg") == 0, "leaf of a path");
        ck(strcmp(bk_leaf("plain"), "plain") == 0, "leaf of a bare name");
        ck(strcmp(bk_leaf("/trailing/"), "") == 0, "leaf after a trailing slash");
    }

    // --- sizes --------------------------------------------------------------
    {
        char s[24];
        bk_size_str(s, sizeof(s), 512);   ck(strcmp(s, "512 B") == 0, "bytes stay bytes");
        bk_size_str(s, sizeof(s), 1024);  ck(strcmp(s, "1.0 KB") == 0, "1024 is 1.0 KB");
        bk_size_str(s, sizeof(s), 1536);  ck(strcmp(s, "1.5 KB") == 0, "1536 is 1.5 KB");
    }

    // --- the default name ---------------------------------------------------
    {
        char n[BK_NAME_MAX];
        g_now = FwTime{2026, 8, 17, 9, 5, 3, 1};
        bk_default_name(n, sizeof(n));
        ck(strcmp(n, "20260817-090503") == 0, "a set clock names the snapshot by date");
        ck(bk_name_ok(n), "and that name passes its own check");

        // A device that has never seen a time server. Every snapshot would
        // otherwise be called 00000000-000000 and the second would collide
        // with the first.
        g_now = FwTime{};
        bk_default_name(n, sizeof(n));
        ck(strcmp(n, "backup-1234567") == 0, "an unset clock falls back to the uptime");
        ck(bk_name_ok(n), "and that name passes too");
    }

    // --- create -------------------------------------------------------------
    {
        fs_populate();
        int rc = run("create", "nightly");
        ck(rc == 0, "create succeeds");
        ck(fs_has("/etc/backups"), "it made the backups directory");
        ck(fs_has("/etc/backups/nightly"), "and the snapshot directory");
        ck(fs_read("/etc/backups/nightly/registry.cfg") == fs_read("/os/registry.cfg"),
           "the registry was copied byte for byte");
        ck(fs_read("/etc/backups/nightly/users.cfg") == fs_read("/os/users.cfg"),
           "so were the accounts");
        ck(fs_read("/etc/backups/nightly/startup.cfg") == fs_read("/etc/startup.cfg"),
           "and the startup file");
        ck(!fs_has("/etc/backups/nightly/tasks.cfg"),
           "a config file that is not there is skipped rather than faked");
        ck(out_has("3 files"), "and it says how many it saved");

        // Twice with the same name would otherwise merge two snapshots into one.
        rc = run("create", "nightly");
        ck(rc != 0, "a second snapshot of the same name is refused");
        ck(out_has("already there"), "and says why");

        rc = run("create", "../escape");
        ck(rc != 0, "a name that would escape the directory is refused");
        ck(!fs_has("/etc/escape"), "and nothing was written outside");
    }

    // --- create with nothing to save ---------------------------------------
    {
        fs_reset();
        fs_dir("/etc");
        int rc = run("create", "empty");
        ck(rc != 0, "a snapshot of nothing fails");
        ck(out_has("Nothing to save"), "and says so");
        ck(!fs_has("/etc/backups/empty"),
           "and leaves no empty directory behind");
    }

    // --- list ---------------------------------------------------------------
    {
        fs_populate();
        run("create", "one");
        run("create", "two");
        int rc = run("list");
        ck(rc == 0, "list succeeds");
        ck(out_has("one"), "it lists the first snapshot");
        ck(out_has("two"), "and the second");
        ck(out_has("3 files"), "with a file count");

        fs_reset();
        run("list");
        ck(out_has("No snapshots yet"),
           "with no backups directory at all it says so rather than failing");
    }

    // --- restore, unconfirmed ----------------------------------------------
    {
        fs_populate();
        run("create", "good");
        fs_file("/os/registry.cfg", "Settings.Theme=broken\n");

        int rc = run("restore", "good");
        ck(rc != 0, "a restore with no confirmation does not go ahead");
        ck(out_has("would overwrite"), "it says what it would do");
        ck(out_has("/os/registry.cfg"), "naming the live files");
        ck(fs_read("/os/registry.cfg") == "Settings.Theme=broken\n",
           "and changes nothing");
    }

    // --- restore, confirmed -------------------------------------------------
    {
        fs_populate();
        run("create", "good");
        std::string original = fs_read("/os/registry.cfg");
        fs_file("/os/registry.cfg", "Settings.Theme=broken\n");
        fs_file("/os/users.cfg", "");

        int rc = run("restore", "good", "-y");
        ck(rc == 0, "a confirmed restore succeeds");
        ck(fs_read("/os/registry.cfg") == original, "the registry came back");
        ck(fs_read("/os/users.cfg") == "dash:hash:admin\n", "so did the accounts");
        ck(out_has("Reboot"), "and it says a reboot is what makes it stick");

        rc = run("restore", "nosuch", "-y");
        ck(rc != 0, "restoring a snapshot that is not there fails");
        ck(out_has("no snapshot"), "and says so");
    }

    // --- restore refuses a stray file --------------------------------------
    {
        fs_populate();
        run("create", "good");
        // Something that is not one of the covered files, sitting in a snapshot
        // directory. Copying it back on the strength of its name would write
        // wherever that name resolved to.
        fs_file("/etc/backups/good/ca.pem", "not a config file\n");
        int rc = run("restore", "good", "-y");
        ck(rc == 0, "the restore still runs");
        ck(out_has("Skipping 'ca.pem'"), "but the stray file is skipped by name");
        ck(!fs_has("/os/ca.pem"), "and nothing was written for it");
    }

    // --- remove -------------------------------------------------------------
    {
        fs_populate();
        run("create", "doomed");
        ck(fs_has("/etc/backups/doomed/registry.cfg"), "the snapshot has files in it");
        int rc = run("remove", "doomed");
        ck(rc == 0, "remove succeeds");
        ck(!fs_has("/etc/backups/doomed"), "the directory is gone");
        ck(!fs_has("/etc/backups/doomed/registry.cfg"), "and so are its contents");
        ck(fs_has("/os/registry.cfg"), "while the live config is untouched");

        rc = run("remove", "doomed");
        ck(rc != 0, "removing it twice fails the second time");
        rc = run("remove", "../..");
        ck(rc != 0, "and an escaping name is refused");
    }

    // --- copying: streamed, no ceiling -------------------------------------
    //
    // The old version read a file into one buffer and wrote it back out, which
    // capped a copy at a single allocation and refused anything larger by name.
    // Streaming through fw_file_copy removed both the ceiling and the two
    // refusals that guarded it. To see the ceiling come back, put a size check
    // back into bk_copy: the first case here goes red.
    {
        fs_reset();
        fs_dir("/etc");

        // Far past the old 32 KB ceiling, and it copies whole.
        std::string big(128u * 1024u, 'x');
        fs_file("/os/registry.cfg", big);
        out_reset();
        long n = bk_copy("/os/registry.cfg", "/etc/copy.cfg");
        ck(n == (long)big.size(), "a file far past the old 32 KB ceiling copies whole");
        ck(fs_read("/etc/copy.cfg") == big, "and every byte of it arrives");

        // A source that is not there is a failure, not a silent empty copy.
        fs_reset();
        fs_dir("/etc");
        out_reset();
        n = bk_copy("/os/registry.cfg", "/etc/copy.cfg");
        ck(n < 0, "a copy of a source that is not there is refused");
        ck(!fs_has("/etc/copy.cfg"), "and nothing is written");

        // A destination that runs out of room mid-stream leaves a truncated file
        // behind. bk_copy has to take that half away.
        fs_reset();
        fs_dir("/etc");
        fs_file("/os/registry.cfg", std::string(4096, 'x'));
        g_fs_full = true;
        out_reset();
        n = bk_copy("/os/registry.cfg", "/etc/copy.cfg");
        g_fs_full = false;
        ck(n < 0, "a copy that runs out of room is refused");
        ck(!fs_has("/etc/copy.cfg"),
           "and the half it wrote is removed, not left as a stub a restore trusts");

        // An empty file is a real answer, not an absence: a config somebody
        // emptied on purpose has to restore empty.
        fs_reset();
        fs_dir("/etc");
        fs_file("/os/registry.cfg", "");
        n = bk_copy("/os/registry.cfg", "/etc/copy.cfg");
        ck(n == 0, "an empty file copies as zero bytes");
        ck(fs_has("/etc/copy.cfg"), "and the copy exists");
        ck(fs_read("/etc/copy.cfg") == "", "and is empty");
    }

    // --- the argument surface ----------------------------------------------
    {
        fs_populate();
        ck(run() == 0, "a bare 'backup' prints the usage rather than failing");
        ck(out_has("backup create"), "and the usage lists create");
        ck(run("help") == 0, "'backup help' works");
        ck(run("info") == 0, "'backup info' works");
        ck(out_has("/os/registry.cfg"), "and info names what it covers");
        ck(run("wibble") != 0, "an unknown subcommand fails");
        ck(out_has("not a backup subcommand"), "and says so");
        ck(run("restore") != 0, "restore with no name fails");
        ck(run("remove") != 0, "remove with no name fails");
        // v1 answered a yes/no question here; the argument spelling stays close
        // to what somebody would have typed.
        fs_populate();
        run("create", "yes-test");
        ck(run("restore", "yes-test", "yes") == 0, "'yes' confirms as well as -y");
        run("create", "yes2");
        ck(run("restore", "yes2", "--yes") == 0, "and so does --yes");
    }

    printf("  %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
