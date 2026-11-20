#include "protect.h"
#include "path.h"

#include <string.h>

// The files themselves. Keep this list short and exact: every entry is a file an
// ordinary account loses access to.
static const char *const kSecret[] = {
    "/os/registry.cfg",     // settings, aliases, sealed Wi-Fi passwords
    "/os/registry.bak",
    "/os/users.cfg",        // accounts and password hashes
    "/os/users.bak",
    "/os/device.key",       // the key saved secrets are sealed under
};
#define N_SECRET ((int)(sizeof(kSecret) / sizeof(kSecret[0])))

// Everything above lives here, so removing or moving it, or matching a wildcard
// inside it, reaches them.
static const char kDir[] = "/os";

#define CANON_MAX 192

// Normalise into `out`, lower case, no trailing slash. A path too long to hold is
// reported as not fitting; the caller treats that as reaching, since a path that
// cannot be judged must not be assumed harmless.
static bool canon(const char *path, char *out) {
    char tmp[CANON_MAX];
    if (strlen(path) >= CANON_MAX) return false;
    if (!path_resolve("/", path, tmp, sizeof(tmp))) return false;
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    for (size_t i = 0; i <= n; i++)
        out[i] = (tmp[i] >= 'A' && tmp[i] <= 'Z') ? (char)(tmp[i] + 32) : tmp[i];
    return true;
}

// The SSH host key, and anything else the server keeps, lives here: a reader
// of the host key can impersonate the device to everybody who trusts it.
static const char kSshDir[] = "/os/ssh";

bool protect_secret(const char *path) {
    if (!path) return false;
    char c[CANON_MAX];
    if (!canon(path, c)) return true;
    for (int i = 0; i < N_SECRET; i++)
        if (strcmp(c, kSecret[i]) == 0) return true;
    // Everything in /os/ssh, and the directory itself (a listing names the files).
    size_t sl = sizeof(kSshDir) - 1;
    if (strncmp(c, kSshDir, sl) == 0 && (c[sl] == 0 || c[sl] == '/')) return true;
    return false;
}

static bool has_wild(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == '*' || s[i] == '?') return true;
    return false;
}

// Case-insensitive for the fixed parts, as the list above is: "/HOME" and
// ".SSH" are other directories on this filesystem, but a guard that only
// stops the exact spelling is one typo from not being a guard.
static bool prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

static bool is_dot_ssh(const char *s, size_t n) {
    if (n != 4) return false;
    return s[0] == '.' && (s[1] | 32) == 's' && (s[2] | 32) == 's' && (s[3] | 32) == 'h';
}

bool protect_system(const char *path) {
    if (!path) return false;
    char c[CANON_MAX];
    if (!canon(path, c)) return true;
    static const char *const kSystem[] = { "/os", "/etc" };
    for (const char *d : kSystem) {
        size_t dl = strlen(d);
        if (strncmp(c, d, dl) == 0 && (c[dl] == 0 || c[dl] == '/')) return true;
    }
    return false;
}

bool protect_home(const char *path, const char *self, bool change) {
    if (!path) return false;
    // Normalised but NOT lower-cased: the user name is matched exactly.
    char c[CANON_MAX];
    if (strlen(path) >= CANON_MAX) return true;
    if (!path_resolve("/", path, c, sizeof(c))) return true;
    size_t n = strlen(c);
    while (n > 1 && c[n - 1] == '/') c[--n] = 0;

    // "/home" itself: removing or moving it takes every .ssh with it.
    if (n == 5 && prefix_ci(c, "/home")) return change;
    if (n < 6 || !prefix_ci(c, "/home/")) return false;

    const char *x = c + 6;
    const char *slash = strchr(x, '/');
    size_t xn = slash ? (size_t)(slash - x) : strlen(x);
    if (has_wild(x, xn)) return true;                  // could be anybody's
    bool mine = self && self[0] && strlen(self) == xn && strncmp(x, self, xn) == 0;
    if (mine) return false;
    // Changing anything in someone else's home: refused. Their .ssh decides who
    // logs in as them, and anything else there can be a script they — an admin,
    // perhaps — run without looking, at boot or by hand.
    if (change) return true;

    // Reading: open, except their .ssh. Their home itself can be listed.
    if (!slash) return false;
    const char *y = slash + 1;
    const char *slash2 = strchr(y, '/');
    size_t yn = slash2 ? (size_t)(slash2 - y) : strlen(y);
    if (is_dot_ssh(y, yn)) return true;                // .ssh or anything in it
    if (!slash2 && has_wild(y, yn)) return true;       // /home/bob/* could be .ssh
    return false;
}

bool protect_reaches(const char *path) {
    if (!path) return false;
    char c[CANON_MAX];
    if (!canon(path, c)) return true;

    if (protect_secret(c)) return true;
    if (strcmp(c, "/") == 0 || strcmp(c, kDir) == 0) return true;

    // A wildcard in the last component of a name directly in "/" or in /os. The
    // first can match the /os directory itself (`rm -r /*`, `rm /o*`), the second
    // the files inside it. A wildcard is expanded one level, so those are the only
    // two directories that can reach a protected file this way.
    const char *slash = strrchr(c, '/');
    if (slash && strpbrk(slash + 1, "*?") != nullptr) {
        size_t dirlen = (size_t)(slash - c);
        if (dirlen == 0) return true;                                    // in "/"
        if (dirlen == sizeof(kDir) - 1 && strncmp(c, kDir, dirlen) == 0) return true;
    }
    return false;
}
