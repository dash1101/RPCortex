// Kernel services: boot sequence, logging, heap accounting.
//
// The C++ analogue of v1's post.py + the RPCortex.py output/log helpers. Small
// on purpose — the kernel's job is to bring the machine up, mount storage, and
// hand control to the shell. Everything with a personality lives above it.
#ifndef RPC_KERNEL_H
#define RPC_KERNEL_H

#include <stdint.h>

// Single source of truth for version + codename, as v1 kept them in
// RPCortex.py. The banner and `ver` both read these.
//
// THE SCHEME: vMAJOR.MINOR.BUILD, with a label when it is not final.
//
//     v2.0.640-beta     a beta of v2.0, built at commit 640
//     v2.0.702-rc       a release candidate
//     v2.0.731          no label: final
//     v2.1.800          the next minor: a platform step (v2.1 is the ESP32 port)
//
// BUILD is the commit count, set by CMake, so it rises on its own, nobody types
// it, and it cannot disagree with the image. Every published release therefore
// has a number no other release has, and a later one is always newer. MINOR moves
// for a step in what the generation runs on or can do (a new architecture, a new
// subsystem), not for each release. MAJOR is a generation, the only thing with a
// codename, and changes with the platform: a new kernel, a new package ABI. A
// release never breaks packages; a change that would is a new MAJOR.
//
// The label (RPC_OS_SUFFIX) says how finished it is: "-beta", "-rc", "-rc2", or
// none. tools/release.py moves MINOR, MAJOR and the label; nothing else is
// hand-edited.
//
// Releases v2.01-beta to v2.05-beta used vMAJOR.ITER (an iteration count). They
// keep their tags; a device running one compares "2.05.635" against "2.0.640"
// and sees the new form as older, so it moves across once with
// `update online --force` (the v2.0 release notes say so).
#define RPC_OS_MAJOR    "2"
#define RPC_OS_MINOR    "0"
#define RPC_OS_SUFFIX   "-beta"                  // "-beta", "-rc", "-rc2", ... or "" when final
#define RPC_OS_CODENAME "Vela II"                // the generation's name

// The build number, set by CMake from the commit count. "0" keeps a host build
// compiling; those do not run CMake's git step.
#ifndef RPC_OS_BUILD
#define RPC_OS_BUILD "0"
#endif

#define RPC_OS_VERNUM   RPC_OS_MAJOR "." RPC_OS_MINOR       // no 'v', no build: "2.0"
// What people read: "v2.0.640-beta". The build is in it, so it is never shown twice.
#define RPC_OS_VERSION  "v" RPC_OS_VERNUM "." RPC_OS_BUILD RPC_OS_SUFFIX
#define RPC_OS_LONG     RPC_OS_VERSION

// What the updater compares: "2.0.640". repo_version_cmp compares number by
// number, so a later build beats an earlier one and "2.1.x" beats every "2.0.x".
// The label is not in it, and neither is the git sha (a dirty tree adds a '+',
// which is not a number).
#define RPC_OS_BUILDVER RPC_OS_VERNUM "." RPC_OS_BUILD

enum LogLevel { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 };

// Logging goes to the serial console now and to a flash ring later. Kept behind
// one call so that second half is a change in one place, not a sweep — the
// mistake v1 made by printing directly all over the tree and then having to
// retrofit a capture buffer.
void klog(LogLevel level, const char *fmt, ...);

// Power-on sequence: clocks, storage mount, subsystem init. Returns false only
// on a failure that makes a usable shell impossible; the caller then drops to a
// recovery prompt rather than looping.
bool kboot(void);

// Save the current wall-clock time so a cold boot restores near it. Call after
// the clock is set or synced, and before a clean reboot.
void clock_persist(void);

// Mark this boot as having reached a usable shell. Until it is called a boot
// counts as failed — three in a row and kboot rebuilds the filesystem rather
// than leaving a device that needs another machine to recover.
void kboot_succeeded(void);

// Give the next boot a clean slate, without claiming this one worked.
void kboot_clear_strikes(void);

// Pretend n boots have already failed, so the recovery ladder can be tested.
void kboot_force_strikes(uint32_t n);

// Whether the recovery ladder decided this boot should load nothing. Read once,
// by the shell task, and combined with a safeboot the person asked for.
bool kboot_maintenance(void);

// Say that the restart about to happen was asked for, so the next boot does not
// announce it as a watchdog reset. Every deliberate restart on this part goes
// through the watchdog, so without this they are indistinguishable from a hang.
void kboot_expect_reboot(void);

// Free heap right now. The single number that mattered most in v1 — here it is
// honest (no GC to lie about fragmentation) and cheap.
uint32_t heap_free(void);

// Total heap arena, for reporting a used/total figure.
uint32_t heap_total(void);

#endif  // RPC_KERNEL_H
