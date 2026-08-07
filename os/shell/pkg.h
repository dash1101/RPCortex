// The package manager — the C++ replacement for pkgmgr.py.
//
// A package is an .app (relocatable ELF that registers commands). Installing one
// copies it under /pkg and records name+version in an index; at boot every
// installed package is loaded so its commands are present, the way v1's
// programs.lp made a package's commands available at the shell. No ZIP, no
// interpreter extraction — a file and a manifest line.
#ifndef RPC_PKG_H
#define RPC_PKG_H

void pkg_init(void);              // ensure /pkg exists; call once at startup
void pkg_load_installed(void);    // load every installed package (boot)
// Install from a file. `quiet` suppresses the success line, for the first-boot
// path that installs the built-in packages before anyone is watching.
// Install a package from a file already on the device.
//
// `consume` means the caller is handing this file over and will not want it
// afterwards, so it can be RENAMED into place rather than copied. That is not a
// tidiness point: a copy needs the package's size in free blocks a SECOND time,
// so installing a 77 KB package needed 154 KB free — and the download only
// reserves eight. On a small filesystem that is how a device ends up full,
// with the install failing at the last step for want of room it never asked
// for. A rename inside one directory costs no blocks at all.
//
// False for a path the user named: `pkg install /home/me/thing.app` must leave
// their file where they put it.
bool pkg_install_file(const char *file, bool quiet, bool consume = false);

// The installed version of `name`, or false when it is not installed. Used by
// `pkg info` and `pkg upgrade` to compare against what the repo offers.
bool pkg_installed_version(const char *name, char *out, unsigned cap);

void pkg_register(void);          // register the `pkg` command

#endif  // RPC_PKG_H
