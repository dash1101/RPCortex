// The CYW43 radio firmware, inflated on its way to the chip. See cyw43_fw.h for
// how the driver is pointed here, and core/fwstream.h for the reader.
//
// Stored as it was, the firmware was 227 KB of every wireless image and the
// single largest thing in it. Deflated it is about 148 KB, which is what took a
// Pico W from a few KB of slot headroom to room for the next feature.
//
// WHAT IT COSTS, and when. Only while the radio comes up: one reader is open at
// a time (the CLM, then the Wi-Fi firmware, then the CLM again), each about
// 3.3 KB of heap, given back before the next opens. Nothing is kept afterwards.
// The time is the inflate, done inside the driver call that brings the chip up;
// cyw43_ll_wifi_on already waits until 150 ms after that call began, which
// covers some of it. DEVICE-UNCONFIRMED: how long the upload takes on each part,
// and that the radio comes up, joins and starts Bluetooth from the inflated image.
#include "cyw43_fw.h"

#include "fwstream.h"
#include "out.h"

static_assert(CYW43_FW_WINDOW == FWSTREAM_WINDOW,
              "tools_cyw43fw.py and core/fwstream.h disagree about the window");

static const FwImage kImage[2] = {
    { "Wi-Fi firmware", cyw43_fw_wifi_z, CYW43_FW_WIFI_ZLEN, CYW43_FW_WIFI_LEN, cyw43_fw_wifi_crc },
    { "Wi-Fi CLM",      cyw43_fw_clm_z,  CYW43_FW_CLM_ZLEN,  CYW43_CLM_LEN,     cyw43_fw_clm_crc  },
};

extern "C" cyw43_fw_stream_t *cyw43_fw_open(int which) {
    if (which != CYW43_FW_WIFI && which != CYW43_FW_CLM) return nullptr;
    FwStream *s = fwstream_open(&kImage[which]);
    if (!s) {
        out_errp("wifi", "No free %u-byte block to unpack the %s; radio not started",
                 (unsigned)fwstream_footprint(), kImage[which].name);
    }
    return s;
}

extern "C" const uint8_t *cyw43_fw_read(cyw43_fw_stream_t *z, size_t n) {
    return fwstream_read(z, (uint32_t)n);
}

extern "C" int cyw43_fw_close(cyw43_fw_stream_t *z) {
    if (!z) return -1;
    const char *name = fwstream_name(z);
    uint32_t at = 0;
    int e = fwstream_close(z, &at);
    // Not all read means the driver stopped early, which it does only when the
    // bus failed under it. That is the driver's to report; the image was fine.
    if (e != FWS_OK && e != FWS_UNREAD) {
        out_errp("wifi", "%s in flash is damaged (%s, byte %u); radio not started",
                 name, fwstream_strerror(e), (unsigned)at);
    }
    return e == FWS_OK ? 0 : -1;
}
