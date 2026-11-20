// Permissions.
//
// Three levels, not a matrix. A command is something anyone signed in may run,
// something that needs an admin, or something that changes the SYSTEM: what it
// runs, who can get in, or whether it is there at all tomorrow. The third kind
// runs only as `sudo <command>`, by an admin, after the account's password (or
// within a few minutes of having typed it). A finer-grained scheme would be more
// impressive and would mostly produce a device where nobody remembers which of
// nine capability bits `reboot` needs.
//
// Why a third level, when admin already existed: being signed in as an admin is
// not proof that anybody knows the password. Autonomy signs root in at boot with
// no prompt at all, and a session left open at the serial port is the same. So
// the commands that would let somebody keep the device (accounts, packages,
// firmware, what starts at boot, the network login) ask for the password every
// time, and the everyday ones (reading, Wi-Fi, reboot) do not.
//
// The level lives on the Command itself rather than in a list somewhere else,
// because a list is a thing that drifts: add a command, forget the list, and it
// is unprotected with nothing to notice. A command whose level depends on its
// arguments (`pkg list` is everyday, `pkg install` is not) names a FORMS entry
// below, and those fail closed: a subcommand nobody listed as harmless needs
// sudo.
//
// `sudo` never grants what the account does not have. It is for an admin proving
// it is them; a normal account typing it is refused, not prompted. A device this
// small has no business having a privilege-escalation path.
#ifndef RPC_PERMS_H
#define RPC_PERMS_H

#include <stdint.h>

enum CmdLevel : uint8_t {
    // Anyone signed in. Reading, listing, looking at your own things.
    LEVEL_USER = 0,
    // Changes the machine for everyone, or reaches outside the user's own data:
    // the registry, the radio, power, the clock, the filesystem outside a home.
    LEVEL_ADMIN = 1,
    // Changes the system: an admin, with `sudo`, and the password. See the
    // table in PERMISSIONS.md.
    LEVEL_SYSTEM = 2,
};

// Which forms of a command change the system, for a command whose level depends
// on its arguments. Each names a rule in perms.cpp. One byte on the Command, so
// the table of commands does not grow.
enum CmdForms : uint8_t {
    FORMS_NONE = 0,
    FORMS_PKG,          // pkg: all but list, search, info, update, repo list, certs
    FORMS_UPDATE,       // update: all but check, and rollback with nothing after it
    FORMS_SSH,          // ssh server: all but status and fingerprint
    FORMS_STARTUP,      // startup: all but list
    FORMS_TASK,         // task: all but list
    FORMS_SERVICE,      // service: all but list and status
    FORMS_AUTONOMY,     // autonomy: all but status
    FORMS_MKACCT,       // mkacct --admin
    FORMS_PASSWD,       // passwd for anyone but yourself
    FORMS_SAFEBOOT,     // safeboot <command>: it runs with sudo at the next start
    FORMS_STOCK,        // stock: all but list
    FORMS_REG,          // reg set of a setting that decides who gets in or what runs
    FORMS_RUN,          // run of a package, where packages run with the OS's privileges
    FORMS_COUNT
};

// What it takes to run a command with these arguments. `user` is who is asking
// (passwd's own-or-someone-else's depends on it). argv[0] is the command.
CmdLevel perms_level(CmdLevel base, uint8_t forms, int argc, char **argv, const char *user);

// The command as named in a message: "pkg install", "ssh server on", "mkacct".
// Up to as many words of argv as the forms entry says name the operation.
void perms_label(uint8_t forms, int argc, char **argv, char *out, uint32_t cap);

// For `sudo -l`: which forms need sudo, in a few words, or nullptr for none.
const char *perms_forms_text(uint8_t forms);

// Whether packages on this board run with the OS's own privileges (no sandbox:
// RP2040). Running one there is the same as handing it the device, so `run` of
// a package file needs sudo. Set once at boot by the platform.
void perms_set_packages_privileged(bool yes);
bool perms_packages_privileged(void);

// Whether the named user may run something at `level` right now. `elevated` is
// true for a command run with `sudo`, and for work the system started from its
// own configuration (startup items, services, scheduled tasks).
bool perms_allows(const char *user, bool is_admin, CmdLevel level, bool elevated);

// A setting that decides who gets in, or what runs at the next start: booting
// straight to a shell as somebody (System.Autonomous), the SSH server (SSH.*),
// aliases every user's shell expands (Alias.*), a command staged for a
// maintenance boot (System.SafeBoot*), and where packages come from (Pkg.Repos).
bool perms_reg_key_system(const char *key);

// May a setting be written by the calling task? The registry holds the keys
// above, and a package can write any key through fw_reg_set.
//
//   restricted (not an admin)  never those keys; from a REMOTE login, only the
//                              package's own settings (Apps.*)
//   an admin, not elevated     anything but those keys
//   elevated                   anything
//
// A task with nobody restricted behind it at boot is "an admin" here.
bool perms_reg_write_ok(const char *key, bool restricted, bool remote, bool elevated);

#endif  // RPC_PERMS_H
