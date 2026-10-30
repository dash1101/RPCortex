// Desc: The whole Nova D1 package as ONE translation unit, for the hosts that drive the runner.
// File: novapkg_all.inc
//
// Every file the package is built from, in the order the tests compile them. The
// screens' state is static to each file, so a harness that wants to drive the
// runner has to be in the same unit as the package; this is the list, kept in one
// place so a new source file is added once. novagui_test and novashots predate
// it and carry their own copies; sim_wasm.cpp is the first user.
//
// Include fakefw_d1.inc (or a wrapper of it) BEFORE this: the package calls
// fw_* and the harness is what answers.
#ifndef NOVAPKG_ALL_INC
#define NOVAPKG_ALL_INC

#include "../apps/novad1/novacore.cpp"
#include "../apps/novad1/novacanvas.cpp"
#include "../apps/novad1/novaicons.cpp"
#include "../apps/novad1/novaboard.cpp"
#include "../apps/novad1/novamodtab.cpp"
#include "../apps/novad1/novalog.cpp"
#include "../apps/novad1/novanotify.cpp"
#include "../apps/novad1/novapower.cpp"
#include "../apps/novad1/display.cpp"
#include "../apps/novad1/novainput.cpp"
#include "../apps/novad1/novaui.cpp"
#include "../apps/novad1/novaapps.cpp"
#include "../apps/novad1/novabootcheck.cpp"
#include "../apps/novad1/novagui_tools.cpp"
#include "../apps/novad1/novagui_system.cpp"
#include "../apps/novad1/novakeys.cpp"
#include "../apps/novad1/novagui_wifi.cpp"
#include "../apps/novad1/novagui_files.cpp"
#include "../apps/novad1/novagui_settings.cpp"
#include "../apps/novad1/novagui_ops.cpp"
#include "../apps/novad1/novagui_apps.cpp"
#include "../apps/novad1/novagui_ble.cpp"
#include "../apps/novad1/novagui_media.cpp"
#include "../apps/novad1/novagui_contact.cpp"
#include "../apps/novad1/novasub.cpp"
#include "../apps/novad1/novair.cpp"
#include "../apps/novad1/novanfc.cpp"
#include "../apps/novad1/novaoui.cpp"
#include "../apps/novad1/novagui_codes.cpp"
#include "../apps/novad1/novagui_radios.cpp"
#include "../apps/novad1/novagui_sensors.cpp"
#include "../apps/novad1/novagui_usb.cpp"
#include "../apps/novad1/novagui_tasks.cpp"
#include "../apps/novad1/novagui.cpp"

#endif  // NOVAPKG_ALL_INC
