// Saving the registry while something else is changing it.
//
// The registry is saved from the shell and, through fw_reg_save, from a package
// task that may be on the other core. Two windows there lost data:
//
//   * save_one measured the table, allocated, then filled it, and wrote the
//     fill's REPORTED size. A key set between the two calls made that size
//     larger than the buffer, and the heap after the buffer went into the file.
//   * The dirty flag was cleared after the write, so a key set while the file
//     was being written was marked saved and never was.
//
// Both are made deterministic here by giving save_one a serializer that grows
// between its calls, and a fake storage_write_file that sets a key mid-write.
// Run under AddressSanitizer, the first one is a heap overflow on the old code.
#include "../core/persist.cpp"

#include <map>
#include <string>
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

// Deterministic; a device backs this with the hardware RNG.
static uint32_t g_seed = 1;
extern "C" uint32_t rpc_rand32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }

// --- a filesystem in a map ---------------------------------------------------
static std::map<std::string, std::string> g_files;
static void (*g_during_write)(void);
static int g_writes;

bool storage_write_file(const char *name, const uint8_t *data, uint32_t len) {
    g_writes++;
    if (g_during_write) { void (*f)(void) = g_during_write; g_during_write = nullptr; f(); }
    g_files[name] = std::string((const char *)data, len);
    return true;
}
uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap) {
    auto it = g_files.find(name);
    if (it == g_files.end()) return 0;
    uint32_t n = (uint32_t)it->second.size() < cap ? (uint32_t)it->second.size() : cap;
    memcpy(buf, it->second.data(), n);
    return n;
}
bool storage_mkdir(const char *) { return true; }

// --- a serializer that changes between its two calls ---------------------------
static int g_calls;
static const char *g_small = "a=1\n";                                  // what the sizing call sees
static const char *g_big   = "a=1\nwifi.ssid=a-network-name-that-is-long\n";   // what the fill sees
static int g_grow_every;     // 0: grow once, then hold still; 1: never holds still

static uint32_t growing_ser(char *buf, uint32_t cap) {
    g_calls++;
    // Odd calls measure, even calls fill. The first fill sees the table bigger
    // than it was measured; after that it holds still, unless told not to.
    const char *t = g_small;
    if (g_grow_every) t = (g_calls % 2 == 0) ? g_big : g_small;
    else if (g_calls >= 2) t = g_big;
    uint32_t need = (uint32_t)strlen(t);
    if (buf && cap) {
        uint32_t n = need < cap - 1 ? need : cap - 1;
        memcpy(buf, t, n);
        buf[n] = 0;
    }
    return need;
}

static void set_a_key(void) { reg_set("System.During_Write", "yes"); }

int main(void) {
    printf("persist_test - saving the registry while it changes\n");

    // --- the table grows between measuring and filling ---
    g_calls = 0; g_grow_every = 0; g_files.clear();
    ck(save_one("/os/t.cfg", growing_ser), "a save that saw the table grow still succeeds");
    ck(g_files["/os/t.cfg"] == g_big, "and writes the whole, current table - nothing from past the buffer");
    ck(g_calls == 4, "having measured again after the fill came back larger");

    // --- a table that never holds still: nothing written, old file kept ---
    g_calls = 0; g_grow_every = 1; g_files.clear();
    g_files["/os/t.cfg"] = "the previous good file\n";
    ck(!save_one("/os/t.cfg", growing_ser), "a table that never holds still is not saved");
    ck(g_files["/os/t.cfg"] == "the previous good file\n", "and the previous good file is left alone");

    // --- a key set while the file is being written stays dirty ---
    g_files.clear();
    reg_set("System.Name", "vela");
    ck(reg_dirty(), "a set marks the registry dirty");
    g_during_write = set_a_key;
    persist_save_registry();
    ck(reg_dirty(), "a key set during the write is still waiting to be saved");
    ck(g_files["/os/registry.cfg"].find("System.Name=vela") != std::string::npos, "the earlier key was saved");
    persist_save_registry();
    ck(!reg_dirty(), "and the next save takes it");
    ck(g_files["/os/registry.cfg"].find("System.During_Write=yes") != std::string::npos,
       "and it is in the file");

    // --- the shadow copy matches the primary ---
    ck(g_files["/os/registry.cfg"] == g_files["/os/registry.bak"], "the shadow copy matches the primary");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
