// TinyUSB's configuration for this firmware.
//
// The SDK's pico_stdio_usb ships a tusb_config.h that fixes CFG_TUD_CDC at 1,
// unconditionally, so it cannot be raised from the command line. This one is
// found first (the firmware's own include path comes before the SDK's) and is
// that file with one change: TWO CDC interfaces. The first is the console, as
// before; stdio uses it and nothing about it changes. The second is the network
// port (netport.cpp): the bridge's own channel, so a computer can give the board
// its network without touching the console (LINK.md, "The network port").
//
// Everything else (MSC, HID, the buffer sizes) is still set in CMakeLists.txt.
#ifndef _RPC_TUSB_CONFIG_H
#define _RPC_TUSB_CONFIG_H

#include "pico/stdio_usb.h"

#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#define CFG_TUD_CDC             (2)

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

#define CFG_TUD_VENDOR            (0)
#endif

#endif
