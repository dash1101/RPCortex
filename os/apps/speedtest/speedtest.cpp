// SpeedTest — how far away the network is, and how fast.
//
//   speedtest              latency, then a download
//   speedtest ping [host]  latency only
//   speedtest down [url]   throughput only
//   speedtest info         what it measures and against what
//
// --- what changed from v1 ---------------------------------------------------
//
// v1 measured latency by timing a TCP connect, and its comment said why: "real
// ICMP isn't available on MicroPython". It is here — `ping` sends genuine echo
// requests through lwIP's raw API — so fw_net_ping is the measurement itself
// rather than a stand-in that also includes a handshake.
//
// The download is still plain HTTP on purpose. TLS costs a handshake and a
// several-kilobyte working set, and neither belongs in a number that is meant
// to describe the link.
//
// The body is never stored. v1 streamed it and discarded it chunk by chunk to
// stay inside 264 KB of RAM; fw_http_measure does the same thing one level
// down, so a megabyte can be pulled on a device with 596 KB of free flash
// without any of it landing there.
#include "rpc_app.h"

RPC_APP_VER("speedtest", "2.0");

// Cloudflare's resolver: anycast, so the reply comes from whichever site is
// nearest, which is what a latency figure should reflect.
#define DEFAULT_PING_HOST "1.1.1.1"
// A megabyte of zeros over plain HTTP, from a host that exists to be downloaded
// from. Small enough not to take a minute on a slow link, big enough that the
// connection setup is not most of the measurement.
#define DEFAULT_DOWN_URL  "http://speedtest.tele2.net/1MB.zip"

#define PING_COUNT   4
#define PING_TIMEOUT 2000

static bool online(void) {
    if (fw_net_connected()) return true;
    fw_printf("Not connected. Use 'wifi connect <ssid>' first.\n");
    return false;
}

// Microseconds as milliseconds with three decimals, without floating point.
static void print_ms(const char *label, uint32_t us) {
    fw_printf("%s%u.%03u ms\n", label, (unsigned)(us / 1000), (unsigned)(us % 1000));
}

static int do_ping(const char *host) {
    fw_printf("\n  Latency to %s\n", host);

    uint32_t best = 0xFFFFFFFFu, worst = 0, total = 0;
    int got = 0;
    for (int i = 0; i < PING_COUNT; i++) {
        if (fw_task_should_stop()) break;
        int us = fw_net_ping(host, PING_TIMEOUT);
        if (us < 0) {
            fw_printf("    %d: %s\n", i + 1,
                      us == -2 ? "no reply" : "could not send");
        } else {
            uint32_t u = (uint32_t)us;
            got++;
            total += u;
            if (u < best)  best  = u;
            if (u > worst) worst = u;
            fw_printf("    %d: %u.%03u ms\n", i + 1,
                      (unsigned)(u / 1000), (unsigned)(u % 1000));
        }
        // A gap between probes, so four samples describe the link over a moment
        // rather than one burst of it.
        if (i + 1 < PING_COUNT) fw_task_sleep_ms(300);
    }

    if (!got) {
        fw_printf("\n  No replies. The host may block ICMP, or the network is down.\n\n");
        return 1;
    }
    fw_printf("\n");
    print_ms("    best    ", best);
    print_ms("    average ", total / (uint32_t)got);
    print_ms("    worst   ", worst);
    if (got < PING_COUNT)
        fw_printf("    loss    %d of %d\n", PING_COUNT - got, PING_COUNT);
    fw_printf("\n");
    return 0;
}

static int do_down(const char *url) {
    fw_printf("\n  Downloading %s\n", url);
    fw_printf("  (the body is discarded as it arrives; nothing is written)\n");

    uint32_t bytes = 0, ms = 0;
    int n = fw_http_measure(url, &bytes, &ms);
    if (n < 0) {
        fw_printf("\n  The download failed. Check the URL is reachable over\n");
        fw_printf("  plain HTTP, and that the network is up.\n\n");
        return 1;
    }
    if (bytes == 0) {
        fw_printf("\n  Nothing arrived.\n\n");
        return 1;
    }

    // Kilobits per second, in integer arithmetic that cannot overflow: bytes is
    // at most 4 GB, and bytes/ms * 8 keeps every intermediate inside 32 bits
    // where bytes*8 would not.
    uint32_t kbps = (bytes / ms) * 8u;          // bytes/ms * 8 == kbit/s
    fw_printf("\n    received  %u KB in %u.%03u s\n",
              (unsigned)(bytes / 1024), (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    if (kbps >= 1000)
        fw_printf("    rate      %u.%02u Mbit/s\n",
                  (unsigned)(kbps / 1000), (unsigned)((kbps % 1000) / 10));
    else
        fw_printf("    rate      %u kbit/s\n", (unsigned)kbps);
    fw_printf("\n");
    return 0;
}

static void info(void) {
    fw_printf("\n  speedtest measures two things separately.\n\n");
    fw_printf("  Latency is real ICMP — the same echo requests 'ping' sends,\n");
    fw_printf("  four of them, 300 ms apart. It is the round trip to %s\n", DEFAULT_PING_HOST);
    fw_printf("  and nothing else: no handshake, no name lookup.\n\n");
    fw_printf("  Throughput streams %s\n", DEFAULT_DOWN_URL);
    fw_printf("  and throws it away as it arrives. The clock starts at the\n");
    fw_printf("  FIRST BYTE, so connecting is not counted against the rate.\n\n");
    fw_printf("  Plain HTTP on purpose: a TLS handshake and its working set\n");
    fw_printf("  describe the device, not the link.\n\n");
    fw_printf("  Pass your own host or http:// URL to test against something\n");
    fw_printf("  closer:  speedtest ping 8.8.8.8   speedtest down <url>\n\n");
}

static void usage(void) {
    fw_printf("Usage:\n");
    fw_printf("  speedtest              latency, then a download\n");
    fw_printf("  speedtest ping [host]  latency only\n");
    fw_printf("  speedtest down [url]   throughput only\n");
    fw_printf("  speedtest info         what it measures\n");
}

static bool arg_is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return !*a && !*b;
}

static int speedtest_cmd(int argc, char **argv) {
    if (argc >= 2 && arg_is(argv[1], "info"))  { info();  return 0; }
    if (argc >= 2 && arg_is(argv[1], "help"))  { usage(); return 0; }

    if (argc >= 2 && arg_is(argv[1], "ping")) {
        if (!online()) return 1;
        return do_ping(argc >= 3 ? argv[2] : DEFAULT_PING_HOST);
    }
    if (argc >= 2 && arg_is(argv[1], "down")) {
        if (!online()) return 1;
        return do_down(argc >= 3 ? argv[2] : DEFAULT_DOWN_URL);
    }
    if (argc >= 2) { usage(); return 1; }

    // No arguments: both, latency first — it is the quick one, and a failure
    // there explains a failure in the download that follows.
    if (!online()) return 1;
    int rc = do_ping(DEFAULT_PING_HOST);
    if (fw_task_should_stop()) return rc;
    int rc2 = do_down(DEFAULT_DOWN_URL);
    return rc ? rc : rc2;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("speedtest", "measure network latency and throughput",
                         speedtest_cmd);
    return 0;
}
