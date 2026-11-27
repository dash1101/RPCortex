// The wireless chip: bringing it up, taking it down, and the one-owner rule
// every caller of it lives under. See radio.h for why this is its own file and
// netown.h for the rule itself.
//
// Moved here from the top of net.cpp unchanged, so that an image can carry
// Bluetooth without Wi-Fi. What radio_up and radio_down did for Wi-Fi alone
// (station mode, power saving, the status cache) is now the three
// wifi_radio_* calls, made at exactly the points that code used to run.

#include "command.h"
#include "out.h"
#include "registry.h"
#include "task.h"
#include "logring.h"
#include "persist.h"
#include "lock.h"
#include "blackbox.h"
#include "netown.h"
#include "radio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "clockcfg.h"

// --- one owner at a time ----------------------------------------------------
//
// NetLock in net.cpp blocks the CORE. That is right for excluding the driver's
// interrupt, and fatal between tasks under cooperative scheduling: a task that
// blocks the core waiting for a lock another task holds has stopped the only
// thing that could release it.
//
// So the rule is that only ONE task is ever inside cyw43 or lwIP. This lock
// enforces it, and it is an RpcLock rather than the driver's — RpcLock YIELDS
// while it waits, so a task holding it keeps running and the waiter does not
// take the core down with it.
//
// Every network OPERATION takes it. Reads do not, because reads no longer
// touch lwIP at all — see NetStatus in net.cpp.
RpcLock g_net_op;

// Exposed so the HTTP transport can hold ownership for the length of a
// connection. It lives here rather than there because this is where the rule
// is stated, and a second lock would defeat the point of having one.
// Taking the network lock also puts the caller on the radio's core, and gives
// it back on release.
//
// The one-owner rule was about TASKS, and that turned out to be half the rule.
// A package task is spawned with no affinity and, measured on a real device,
// runs on core 1 essentially always — so "one task at a time" was satisfied
// while every call was still arriving on the wrong core. Refusing was the
// honest thing to do while there was no way to move a task; now there is.
//
// The old affinity is remembered per task so a package that was free to run
// anywhere goes back to being free afterwards, rather than being quietly
// pinned to core 0 for the rest of its life by having once used the network.
// Declared below, with the radio lifecycle they belong to.
bool g_radio_up;
static uint32_t g_radio_core;

#define NET_OWNERS 8
static struct { int pid; TaskAffinity was; } g_net_prev[NET_OWNERS];

void net_op_acquire(void) {
    lock_acquire(&g_net_op);

    uint32_t target = g_radio_up ? g_radio_core : 0;
    if (task_this_core() == target) return;

    int me = task_self();
    int slot = -1;
    for (int i = 0; i < NET_OWNERS; i++)
        if (!g_net_prev[i].pid) { slot = i; break; }

    // No room to remember where this task came from. Migrating anyway would
    // pin it to core 0 for the rest of its life, since nothing would ever put
    // its affinity back — a slow leak that ends with every task on one core.
    // Better to leave it where it is: net_core_ok then refuses the call with a
    // message, which is visible and recoverable.
    if (slot < 0) {
        out_warnp("net", "Too many tasks using the network at once.");
        return;
    }

    g_net_prev[slot].pid = me;
    g_net_prev[slot].was = task_affinity(me);

    // A failure here is not fatal either: net_core_ok still refuses the call,
    // so the outcome is a clear message rather than corruption.
    task_migrate_to(target);
}

void net_op_release(void) {
    int me = task_self();
    for (int i = 0; i < NET_OWNERS; i++) {
        if (g_net_prev[i].pid != me) continue;
        // Only on the OUTERMOST release. The lock is recursive, and restoring
        // on an inner one would drop this task back to core 1 halfway through
        // somebody else's connection.
        if (!lock_held_once(&g_net_op)) break;
        task_set_affinity(me, g_net_prev[i].was);
        g_net_prev[i].pid = 0;
        break;
    }
    lock_release(&g_net_op);
}

bool net_op_held(void) { return lock_held_by_me(&g_net_op); }

// The tripwire. One counter and the caller that tripped it first.
//
// Every entry into cyw43 or lwIP is supposed to be under the operation lock,
// and three files have quietly not been. Counting is enough: the count is zero
// on a correct build, so `wifi` says nothing, and the first time somebody adds
// a fourth the number appears with an address to look up.
static uint32_t g_unowned;
static uint32_t g_unowned_pc;

// The address recorded is where this call returns to, which after the guard's
// constructor is inlined is the offending scope itself. addr2line on the ELF
// names the line.
void net_unowned_note(void) {
    if (!g_unowned) g_unowned_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    g_unowned++;
}
uint32_t net_unowned_count(void) { return g_unowned; }

// --- radio lifecycle --------------------------------------------------------

// Bring the chip up on demand. cyw43_arch_init loads the firmware blob over
// SPI, which takes a moment and is why this is not done at boot.
// The privacy latch. Checked HERE rather than in the command, because every
// path that needs the chip comes through this one — including the background
// join at boot, which would otherwise bring the radio up behind a lock the user
// set deliberately. A switch that only stops the polite callers is not a switch.
bool radio_locked(void) {
    return strcmp(reg_get("System.RadioLock", "false"), "true") == 0;
}

bool radio_up(void) {
    if (radio_locked()) {
        out_err("Radios are locked off. 'radio on' to release the lock.");
        return false;
    }
    if (g_radio_up) return true;

    // Ownership HERE, not only in the callers. Bringing the chip up is the
    // single most core-sensitive thing in this file — g_radio_core is set to
    // whatever core runs it, and every later call has to match — and two of the
    // three callers reach it without holding anything (`radio on`, and
    // bluetooth through net_radio_up). Recursive, so the caller that does hold
    // it pays nothing.
    NetOwn own;
    if (!own.ok) {
        out_err("Could not reach the wireless chip from this task.");
        return false;
    }
    join_phase("cyw43_arch_init");
    if (cyw43_arch_init() != 0) {
        out_err("Could not start the wireless chip.");
        return false;
    }
    // Station mode and the power-save setting, when Wi-Fi is in the image. It
    // is also where the firmware goes over the bus, and where a chip that did
    // not start is found (net.cpp).
    if (!wifi_radio_starting()) return false;

    g_radio_core = task_this_core();
    g_radio_up = true;

    // The first status reading, while this core is by definition the radio's
    // core (net.cpp says why it cannot wait).
    wifi_radio_started();

    join_phase("radio up");
    return true;
}

// Shared with bt.cpp, which needs the same chip up and must not decide for
// itself whether it already is. Bluetooth calling cyw43_arch_init directly
// re-initialised a chip the WiFi side had already started — a second firmware
// download over a bus with a live async_context on it — and the driver hard
// asserted on the bus errors that produced. It also walked straight past the
// radio lock, which is the "switch that only stops the polite callers" this
// file warns about a few lines up.
bool net_radio_up(void) { return radio_up(); }

// Is the radio up RIGHT NOW — a pure question, with no power to change the
// answer. net_radio_up() above is misnamed for a query: it is radio_up(), an
// ACTIVATOR that brings the chip up (and errors, or faults mid-teardown, when
// it cannot). A per-frame status read must never do that. fw_power_source draws
// a battery icon every frame and only wants to know whether it is safe to read
// VBUS off the chip; it asks THIS. The freeze it caused: incognito took the
// radio down, the next status frame called the activator, and it tried to
// re-initialise the chip from the render task's core straight into the driver's
// own teardown — the "second firmware download on a live async_context" this
// file warns about a few lines up — and the GUI task faulted.
bool net_radio_is_up(void) { return g_radio_up; }

// Raised every time the radio goes down. Connections are closed just before
// (radio_down), and anything that opened before a change treats itself as
// gone: a quick test that needs no lock.
static volatile uint32_t g_radio_gen = 1;
uint32_t net_radio_gen(void) { return g_radio_gen; }

// The one-owner rule is about TASKS. This is the other half of it: the core.
//
// g_net_op guarantees one task at a time inside the driver, which is not the
// same as one CORE — a task with AFFINITY_ANY can be scheduled onto either, and
// the driver only works on the one that initialised it.
//
// It DOES fire. A package doing its own networking from an unpinned task was
// the hypothetical this was written for, and TCP for packages made it real:
// fw_tcp_* and fw_net_ping are reached from a task spawned with no affinity,
// which on this chip means core 1 almost always.
//
// net_op_acquire migrates the caller here rather than leaving it to fail, so by
// the time anything asks this the answer is normally yes. It is still asked,
// because the migration can be refused — the table that remembers where a task
// came from is finite, and a migration that cannot be undone later is worse
// than one not made. What is NOT allowed is going ahead anyway: see netown.h
// for what the driver does to a caller on the wrong core.
bool net_core_ok(void) {
    return !g_radio_up || task_this_core() == g_radio_core;
}

int radio_quiesce_begin(void) {
    net_op_acquire();
    if (!g_radio_up) return 1;          // the chip is off: nothing on the bus
    if (!net_core_ok()) { net_op_release(); return 0; }
    cyw43_arch_lwip_begin();             // the driver's lock: background work waits
    return 2;
}

// The bus's state machine is the SDK's own (cyw43_bus_pio_spi.c) and not
// exported, so it is found by what it drives: the one claimed state machine
// whose side-set pin is the chip's clock line.
static bool bus_sm(PIO *pio_out, uint *sm_out) {
    for (uint p = 0; p < NUM_PIOS; p++) {
        PIO pio = PIO_INSTANCE(p);
        for (uint sm = 0; sm < NUM_PIO_STATE_MACHINES; sm++) {
            if (!pio_sm_is_claimed(pio, sm)) continue;
            uint32_t pc = pio->sm[sm].pinctrl;
            uint base = (pc & PIO_SM0_PINCTRL_SIDESET_BASE_BITS) >> PIO_SM0_PINCTRL_SIDESET_BASE_LSB;
            uint count = (pc & PIO_SM0_PINCTRL_SIDESET_COUNT_BITS) >> PIO_SM0_PINCTRL_SIDESET_COUNT_LSB;
#if PICO_PIO_VERSION > 0
            base += pio_get_gpio_base(pio);
#endif
            if (count && base == CYW43_PIN_WL_CLOCK) { *pio_out = pio; *sm_out = sm; return true; }
        }
    }
    return false;
}

static uint32_t g_bus_div = CYW43_PIO_CLOCK_DIV_INT;

uint32_t radio_bus_hz(void) { return clock_get_hz(clk_sys) / g_bus_div; }

uint32_t radio_clock_changed(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t div = clockcfg_bus_div(sys, SYS_CLK_HZ / CYW43_PIO_CLOCK_DIV_INT);
    g_bus_div = div;
    cyw43_set_pio_clkdiv_int_frac8(div, 0);    // for the next time the chip starts
    if (!g_radio_up) return sys / div;
    PIO pio; uint sm;
    if (!bus_sm(&pio, &sm)) {
        log_addf(LOG_K_WARN, "radio: bus state machine not found; divider left as it was");
        return 0;
    }
    pio_sm_set_clkdiv_int_frac8(pio, sm, (uint16_t)div, 0);
    pio_sm_clkdiv_restart(pio, sm);
    return sys / div;
}

void radio_quiesce_end(int token) {
    if (token == 2) cyw43_arch_lwip_end();
    if (token) net_op_release();
}

void radio_down(void) {
    if (!g_radio_up) return;
    // Wi-Fi's connections are closed first, while lwIP may still be entered;
    // net.cpp says why.
    wifi_radio_stopping();
    g_radio_gen++;
    cyw43_arch_deinit();
    g_radio_up = false;
    // The Wi-Fi status cache goes with it; net.cpp says why.
    wifi_radio_stopped();
}

// radio [on|off|status] — the hard stop for every radio.
//
// Persistent on purpose: a privacy switch that forgets itself over a reboot is
// not one. `airplane` is the same command, as it was in v1.
static int cmd_radio(int argc, char **argv) {
    const char *a = argc > 1 ? argv[1] : "status";

    if (strcmp(a, "off") == 0 || strcmp(a, "lock") == 0) {
        reg_set("System.RadioLock", "true");
        persist_save_dirty();
        radio_down();          // down now, not just refused next time
        out_ok("Radios locked off. This survives a reboot.");
        log_add(LOG_K_WARN, "radio: locked off");
        return 0;
    }
    if (strcmp(a, "on") == 0 || strcmp(a, "unlock") == 0) {
        reg_set("System.RadioLock", "false");
        persist_save_dirty();
        out_ok("Radio lock released.");
        log_add(LOG_K_INFO, "radio: lock released");
        return 0;
    }
    if (strcmp(a, "status") == 0) {
        if (radio_locked()) out_warnp("radio", "Locked off.");
        else                out_okp("radio", "Available.");
        return 0;
    }

    out_multi("Usage: radio off      lock every radio down (survives a reboot)");
    out_multi("       radio on       release the lock");
    out_multi("       radio status   show the current state");
    return a[0] ? 1 : 0;
}

void radio_register(void) {
    static const Command cr{"radio", "lock every radio off, or release it", cmd_radio,
                            nullptr, LEVEL_ADMIN};
    cmd_register(&cr);
    cmd_alias("airplane", "radio");
}
