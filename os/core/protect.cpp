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
    path_resolve("/", path, tmp, sizeof(tmp));
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    for (size_t i = 0; i <= n; i++)
        out[i] = (tmp[i] >= 'A' && tmp[i] <= 'Z') ? (char)(tmp[i] + 32) : tmp[i];
    return true;
}

bool protect_secret(const char *path) {
    if (!path) return false;
    char c[CANON_MAX];
    if (!canon(path, c)) return true;
    for (int i = 0; i < N_SECRET; i++)
        if (strcmp(c, kSecret[i]) == 0) return true;
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
