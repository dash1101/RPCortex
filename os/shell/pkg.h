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
void pkg_register(void);          // register the `pkg` command

#endif  // RPC_PKG_H
