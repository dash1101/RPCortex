// The scheduler's platform seam for tests that link interrupt.cpp but are not
// about scheduling. intr_check yields, which pulls task.cpp in; these give it
// the symbols it needs. Nothing here ever switches, because those tests never
// call task_init — and reschedule returns immediately when it has not.
#include "task.h"
#include <stdio.h>
#include <stdlib.h>

extern "C" {
// Single-threaded host: the cross-core critical section has nothing to guard.
void lock_hw_init(void) {}
void lock_hw_enter(void) {}
// One "core" on the host; the tests exercise the scheduler, not the silicon.
unsigned lock_hw_core(void) { return 0; }
uint32_t task_now_us(void) { return 0; }
void lock_hw_exit(void)  {}

void *task_ctx_init(void *stack_top, TaskEntry) { return stack_top; }
void  task_ctx_switch(void **, void *, volatile bool *) {}
uint32_t task_now_ms(void)     { return 0; }
uint32_t task_core_count(void) { return 1; }
uint32_t task_this_core(void)  { return 0; }

// The scheduler's new safety hooks. On the host there is no watchdog and no
// stack to overflow (ucontext gives each task a generous one), but the symbols
// have to exist for anything linking task.cpp.
void task_watchdog_start(void) {}
void task_watchdog_feed(void)  {}
// The host has a large stack it does not manage; report plenty of room so the
// guard never fires there.
uint32_t task_main_stack_headroom(void) { return 1024 * 1024; }
uint32_t task_main_stack_size(void)     { return 1024 * 1024; }
void task_main_stack_paint(void) {}
uint32_t task_main_stack_used(void)     { return 0; }
void task_stack_overflow(const char *, uint32_t) {
    fprintf(stderr, "  *** task_stack_overflow called on the host ***\n");
    abort();
}

// No protection hardware here. task_test.cpp records these instead of ignoring
// them, because it is the test that cares WHEN they are called; everything else
// linking task.cpp just needs the symbols to exist.
void task_stack_guard_set(const void *, uint32_t) {}
void task_app_mem_apply(const TaskAppMem *) {}
void task_slot_recycled(int) {}

// No sandbox on the host, so a task is always on its own stack.
extern "C" bool sandbox_guard_stack(int, void **, unsigned *) { return false; }

}
