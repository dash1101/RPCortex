// The real describe(), reachable from a test.
//
// os/shell/apps.cpp turns a LoadedApp into the TaskAppMem the protection unit
// is programmed from, and realapp_test carries a shadow of that arithmetic so it
// can tell whether the spans the loader produces are ones the hardware will
// take. The shadow was a COPY, which is deliberate and is worth keeping: a
// helper that called the loader's own placement would agree with it whatever
// either of them did. What it could not do is notice describe() itself changing.
// A shadow of code nobody is comparing against is a shadow of whatever that code
// used to be.
//
// So the two are now compared, and this file is the whole of how. describe() is
// static, which is right — nothing outside apps.cpp should be programming
// regions — so the source is INCLUDED rather than linked, exactly as
// websearch_test, btadv_test and radio_test include the package sources whose
// interesting parts are static.
//
// The stubs below exist for one reason: apps.cpp is a shell file and refers to
// the rest of the shell, and none of that is wanted here. They are the same
// seven apps_test.cpp already carries. If a new one appears, the link fails by
// name and this list gains a line — which is a better failure than a linker
// flag quietly collecting the difference away.
#include "../../loader-spike/firmware/loader.h"
#include "../core/task.h"

uint32_t heap_free(void)  { return 1000; }
uint32_t heap_total(void) { return 2000; }
bool storage_open_source(const char *, AppSource *, void **) { return false; }
void storage_close_source(void *) {}
extern "C" void api_set_current_app(void *) {}
extern "C" volatile const char *g_current_app = nullptr;
extern "C" int fault_report_contained(void) { return 0; }

#include "../shell/apps.cpp"

// The one thing this file is for.
void host_app_describe(const LoadedApp *a, TaskAppMem *m) { describe(a, m); }
