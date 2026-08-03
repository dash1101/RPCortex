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
bool pkg_install_file(const char *file, bool quiet);

// The installed version of `name`, or false when it is not installed. Used by
// `pkg info` and `pkg upgrade` to compare against what the repo offers.
bool pkg_installed_version(const char *name, char *out, unsigned cap);

void pkg_register(void);          // register the `pkg` command

#endif  // RPC_PKG_H
