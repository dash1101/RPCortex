#include "users.h"
#include "sha256.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct User {
    char name[USR_NAME_MAX];
    char cred[USR_CRED_MAX];    // "salthex$hashhex", or "NOPASS"
    char home[USR_HOME_MAX];
    UserRole role;
};

static User    g_users[USR_MAX];
static uint32_t g_count;
static bool     g_dirty;

static const char *NOPASS = "NOPASS";
#define SALT_BYTES 16

static int find(const char *name) {
    for (uint32_t i = 0; i < g_count; i++)
        if (strcmp(g_users[i].name, name) == 0) return (int)i;
    return -1;
}

// The one place the hashing scheme lives. To harden to PBKDF2 later, iterate the
// SHA here; nothing else in the file needs to change, and old records stay
// verifiable if the iteration count is encoded in the cred.
static void hash_password(const uint8_t *salt, const char *password,
                          char out_hex[SHA256_DIGEST_LEN * 2 + 1]) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, salt, SALT_BYTES);
    sha256_update(&c, password, strlen(password));
    uint8_t digest[SHA256_DIGEST_LEN];
    sha256_final(&c, digest);
    hex_encode(digest, SHA256_DIGEST_LEN, out_hex);
}

static void make_cred(const char *password, char cred[USR_CRED_MAX]) {
    uint8_t salt[SALT_BYTES];
    for (int i = 0; i < SALT_BYTES; i += 4) {
        uint32_t r = rpc_rand32();
        salt[i]   = (uint8_t)r;
        salt[i+1] = (uint8_t)(r >> 8);
        salt[i+2] = (uint8_t)(r >> 16);
        salt[i+3] = (uint8_t)(r >> 24);
    }
    char salthex[SALT_BYTES * 2 + 1];
    char hashhex[SHA256_DIGEST_LEN * 2 + 1];
    hex_encode(salt, SALT_BYTES, salthex);
    hash_password(salt, password, hashhex);
    snprintf(cred, USR_CRED_MAX, "%s$%s", salthex, hashhex);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void reg_note_home(const char *name, char *out, uint32_t cap) {
    snprintf(out, cap, "/home/%s/", name);
}

bool users_add(const char *name, const char *password, bool admin, bool nopass) {
    if (!name || strlen(name) >= USR_NAME_MAX) return false;
    if (find(name) >= 0 || g_count >= USR_MAX) return false;
    if (!nopass && (!password || strlen(password) == 0)) return false;
    User &u = g_users[g_count];
    strcpy(u.name, name);
    if (nopass) strcpy(u.cred, NOPASS);
    else        make_cred(password, u.cred);
    reg_note_home(name, u.home, USR_HOME_MAX);
    u.role = admin ? ROLE_ADMIN : ROLE_USER;
    g_count++;
    g_dirty = true;
    return true;
}

bool users_verify(const char *name, const char *password) {
    int i = find(name);
    if (i < 0) return false;
    if (strcmp(g_users[i].cred, NOPASS) == 0) return true;   // guest: any password
    if (!password) return false;
    // Split "salthex$hashhex".
    const char *dollar = strchr(g_users[i].cred, '$');
    if (!dollar) return false;
    uint32_t saltlen = (uint32_t)(dollar - g_users[i].cred);
    if (saltlen != SALT_BYTES * 2) return false;
    uint8_t salt[SALT_BYTES];
    for (int b = 0; b < SALT_BYTES; b++) {
        int hi = hexval(g_users[i].cred[b*2]), lo = hexval(g_users[i].cred[b*2+1]);
        if (hi < 0 || lo < 0) return false;
        salt[b] = (uint8_t)((hi << 4) | lo);
    }
    char want[SHA256_DIGEST_LEN * 2 + 1];
    hash_password(salt, password, want);
    // Constant-time-ish compare: length is fixed and we do not early-out on the
    // first differing byte, so timing does not leak how much of the hash matched.
    const char *have = dollar + 1;
    if (strlen(have) != SHA256_DIGEST_LEN * 2) return false;
    uint8_t diff = 0;
    for (int k = 0; k < SHA256_DIGEST_LEN * 2; k++) diff |= (uint8_t)(want[k] ^ have[k]);
    return diff == 0;
}

bool users_exists(const char *name)  { return find(name) >= 0; }
bool users_is_admin(const char *name){ int i=find(name); return i>=0 && g_users[i].role==ROLE_ADMIN; }
bool users_is_nopass(const char *name){int i=find(name); return i>=0 && strcmp(g_users[i].cred,NOPASS)==0;}

bool users_set_password(const char *name, const char *password) {
    int i = find(name);
    if (i < 0 || !password || strlen(password) == 0) return false;
    make_cred(password, g_users[i].cred);
    g_dirty = true;
    return true;
}

bool users_remove(const char *name) {
    if (!name) return false;
    if (strcmp(name, "root") == 0 || strcmp(name, "guest") == 0) return false;  // protected
    int i = find(name);
    if (i < 0) return false;
    for (uint32_t j = (uint32_t)i; j + 1 < g_count; j++) g_users[j] = g_users[j+1];
    g_count--;
    g_dirty = true;
    return true;
}

uint32_t users_count(void) { return g_count; }
const char *users_name_at(uint32_t i) { return i < g_count ? g_users[i].name : nullptr; }

void users_clear(void) { g_count = 0; g_dirty = true; }

void users_load(const char *text, uint32_t len) {
    g_count = 0;
    if (!text) { g_dirty = false; return; }
    uint32_t i = 0;
    char line[USR_NAME_MAX + USR_CRED_MAX + USR_HOME_MAX + 16];
    while (i < len && g_count < USR_MAX) {
        uint32_t j = 0;
        while (i < len && text[i] != '\n' && j < sizeof(line) - 1) line[j++] = text[i++];
        while (i < len && text[i] != '\n') i++;
        if (i < len) i++;
        line[j] = 0;
        if (line[0] == 0 || line[0] == '#') continue;
        // name,cred,home,role
        char *f1 = strchr(line, ','); if (!f1) continue; *f1++ = 0;
        char *f2 = strchr(f1, ',');   if (!f2) continue; *f2++ = 0;
        char *f3 = strchr(f2, ',');   // role optional (3-field legacy: root=admin)
        UserRole role = ROLE_USER;
        if (f3) { *f3++ = 0; if (strcmp(f3, "admin") == 0) role = ROLE_ADMIN; }
        else if (strcmp(line, "root") == 0) role = ROLE_ADMIN;
        if (strlen(line) >= USR_NAME_MAX || strlen(f1) >= USR_CRED_MAX ||
            strlen(f2) >= USR_HOME_MAX) continue;
        User &u = g_users[g_count++];
        strcpy(u.name, line); strcpy(u.cred, f1); strcpy(u.home, f2); u.role = role;
    }
    g_dirty = false;
}

uint32_t users_serialize(char *buf, uint32_t cap) {
    uint32_t need = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        const char *role = g_users[i].role == ROLE_ADMIN ? "admin" : "user";
        int n = snprintf(nullptr, 0, "%s,%s,%s,%s\n",
                         g_users[i].name, g_users[i].cred, g_users[i].home, role);
        if (n < 0) continue;
        if (buf && need + (uint32_t)n < cap)
            snprintf(buf + need, cap - need, "%s,%s,%s,%s\n",
                     g_users[i].name, g_users[i].cred, g_users[i].home, role);
        need += (uint32_t)n;
    }
    if (buf && cap) buf[need < cap ? need : cap - 1] = 0;
    return need;
}

bool users_dirty(void) { return g_dirty; }
void users_mark_clean(void) { g_dirty = false; }
