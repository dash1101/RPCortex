// The scheduler's platform seam for tests that link interrupt.cpp but are not
// about scheduling. intr_check yields, which pulls task.cpp in; these give it
// the symbols it needs. Nothing here ever switches, because those tests never
// call task_init — and reschedule returns immediately when it has not.
#include "task.h"

extern "C" {
void *task_ctx_init(void *stack_top, TaskEntry) { return stack_top; }
void  task_ctx_switch(void **, void *) {}
uint32_t task_now_ms(void)     { return 0; }
uint32_t task_core_count(void) { return 1; }
uint32_t task_this_core(void)  { return 0; }
}
