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
#define RPC_OS_VERSION  "v2.2.0"
#define RPC_OS_CODENAME "Vela II"

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
