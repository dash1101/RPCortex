// A2DP sink — the device as a Bluetooth speaker.
//
// This is the thing v1 could not do at any level of effort. MicroPython's
// `bluetooth` module is BLE only, so there was no route to the Classic profiles
// at all without replacing the firmware. btstack has them, so here they are.
//
// It lives in the OS rather than in a package, and that is a decision rather
// than convenience. A2DP is four layers deep — L2CAP, AVDTP, A2DP, then SBC —
// and exposing enough of btstack through the package ABI to build it outside
// would mean exposing most of btstack. So the OS owns the pipeline and a
// package gets a handful of calls: play, pause, what is playing, how loud.
//
// The pipeline, and where each piece runs:
//
//   the phone     sends SBC frames over Bluetooth
//   btstack       hands them to handle_media, in the radio's IRQ
//   the decoder   turns them into 16-bit PCM, also there
//   the ring      holds decoded samples; the only thing shared across contexts
//   the output    pulls from the ring on a timer at the sample rate
//
// The ring is the whole design. Decoding happens in bursts whenever a packet
// arrives and playback needs one sample every 23 microseconds, so something has
// to absorb the difference — and it has to do it without a lock, because one
// side is an interrupt and taking a lock there would stall the radio.
//
// DEVICE-UNCONFIRMED. It builds and the ring is host-tested; no phone has been
// connected to it.

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "persist.h"
#include "pcmring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "btstack.h"

void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);
bool bt_stack_up(void);

// --- the ring ---------------------------------------------------------------
//
// Sized for about a fifth of a second at 44.1 kHz stereo. Long enough that a
// late packet does not break the sound, short enough that pausing does not
// leave a fifth of a second of stale audio playing afterwards.
#define PCM_RING_SAMPLES 8192
static int16_t   g_pcm[PCM_RING_SAMPLES];
static PcmRing   g_ring;

// --- decoding ---------------------------------------------------------------

static btstack_sbc_decoder_bluedroid_t g_sbc_state;
static const btstack_sbc_decoder_t    *g_sbc;

static uint32_t g_sample_rate = 44100;
static int      g_channels    = 2;
static bool     g_playing;
static bool     g_connected;
static char     g_peer[24];
static uint32_t g_underruns;

// Called by the decoder with finished PCM. Runs in the radio's interrupt, so it
// does the least it can: copy into the ring and return. Anything slower here is
// time the radio is not servicing the link.
static void on_pcm(int16_t *data, int num_samples, int num_channels,
                   int sample_rate, void *) {
    g_sample_rate = (uint32_t)sample_rate;
    g_channels    = num_channels;
    pcm_ring_write(&g_ring, data, (uint32_t)(num_samples * num_channels));
}

// --- output -----------------------------------------------------------------
//
// PWM, because it needs no hardware anyone has to buy: a speaker and a resistor
// on one pin makes a sound. It is not good audio — one bit of carrier noise and
// eight bits of depth — and I2S to a real DAC is the upgrade, which is why the
// ring is separate from the thing draining it.
//
// The alarm fires at the sample rate and writes one sample. At 44.1 kHz on a
// 240 MHz part that is about 5400 cycles between interrupts, which is a lot of
// room for the handful of instructions below.

#define AUDIO_PWM_WRAP 255      // eight bits, so the carrier stays well above hearing

static int      g_audio_pin = -1;
static uint     g_audio_slice;
static uint32_t g_volume = 60;          // percent
static repeating_timer_t g_audio_timer;
static bool     g_audio_running;

static bool audio_tick(repeating_timer_t *) {
    int16_t s[2];
    uint32_t got = pcm_ring_read(&g_ring, s, (uint32_t)g_channels);
    if (got == 0) {
        g_underruns++;
        return true;                     // silence is better than a click
    }

    // Mix to mono, because there is one pin. Averaging rather than taking the
    // left channel: a track mixed hard to one side would otherwise be silent.
    int32_t v = (g_channels == 2) ? ((int32_t)s[0] + s[1]) / 2 : s[0];

    v = v * (int32_t)g_volume / 100;
    // 16-bit signed to 8-bit unsigned, which is what the PWM level wants.
    int32_t level = (v + 32768) >> 8;
    if (level < 0) level = 0;
    if (level > AUDIO_PWM_WRAP) level = AUDIO_PWM_WRAP;
    pwm_set_gpio_level((uint)g_audio_pin, (uint16_t)level);
    return true;
}

extern "C" int fw_gpio_usable(unsigned pin);

static bool audio_start(int pin) {
    if (!fw_gpio_usable((unsigned)pin)) return false;
    if (g_audio_running) return true;

    gpio_set_function((uint)pin, GPIO_FUNC_PWM);
    g_audio_slice = pwm_gpio_to_slice_num((uint)pin);
    pwm_config c = pwm_get_default_config();
    // No divider: the carrier wants to be as far above hearing as it can get,
    // and at 240 MHz over 256 steps that is nearly a megahertz.
    pwm_config_set_wrap(&c, AUDIO_PWM_WRAP);
    pwm_init(g_audio_slice, &c, true);
    pwm_set_gpio_level((uint)pin, AUDIO_PWM_WRAP / 2);   // mid rail, not silence-at-zero

    g_audio_pin = pin;
    // Negative period means "from the start of the last call", so a slow tick
    // does not drift the sample rate slower still.
    int64_t us = -(int64_t)(1000000 / g_sample_rate);
    g_audio_running = add_repeating_timer_us(us, audio_tick, nullptr, &g_audio_timer);
    return g_audio_running;
}

static void audio_stop(void) {
    if (!g_audio_running) return;
    cancel_repeating_timer(&g_audio_timer);
    g_audio_running = false;
    if (g_audio_pin >= 0) {
        pwm_set_gpio_level((uint)g_audio_pin, 0);
        pwm_set_enabled(g_audio_slice, false);
    }
    pcm_ring_clear(&g_ring);
}

// --- the profile ------------------------------------------------------------

static uint8_t g_sdp_a2dp[150];
static uint8_t g_sdp_avrcp[200];
static uint8_t g_media_sbc_codec[4];
static avdtp_stream_endpoint_t *g_endpoint;
static uint16_t g_a2dp_cid;
static uint8_t  g_seid;

static void handle_media(uint8_t seid, uint8_t *packet, uint16_t size) {
    (void)seid;
    // Strip the media header and hand the rest to the decoder.
    int pos = 12;                     // RTP header
    if (size <= pos) return;
    g_sbc->decode_signed_16(&g_sbc_state, 0, packet + pos + 1, size - pos - 1);
}

static void a2dp_handler(uint8_t type, uint16_t, uint8_t *packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            bd_addr_t addr;
            a2dp_subevent_stream_established_get_bd_addr(packet, addr);
            g_a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            g_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            g_connected = true;
            snprintf(g_peer, sizeof(g_peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            log_addf(LOG_K_OK, "btaudio: stream from %s", g_peer);
            break;
        }
        case A2DP_SUBEVENT_STREAM_STARTED:
            g_playing = true;
            pcm_ring_clear(&g_ring);      // start from silence, not from stale audio
            if (g_audio_pin >= 0) audio_start(g_audio_pin);
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
        case A2DP_SUBEVENT_STREAM_STOPPED:
            g_playing = false;
            audio_stop();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            g_playing = false;
            g_connected = false;
            g_peer[0] = 0;
            audio_stop();
            log_add(LOG_K_INFO, "btaudio: stream released");
            break;

        default:
            break;
    }
}

bool btaudio_start(int pin) {
    if (!bt_stack_up()) return false;

    pcm_ring_init(&g_ring, g_pcm, PCM_RING_SAMPLES);

    g_sbc = btstack_sbc_decoder_bluedroid_init_instance(&g_sbc_state);
    g_sbc->configure(&g_sbc_state, SBC_MODE_STANDARD, on_pcm, nullptr);

    l2cap_init();
    a2dp_sink_init();
    a2dp_sink_register_packet_handler(&a2dp_handler);
    a2dp_sink_register_media_handler(&handle_media);

    g_endpoint = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        g_media_sbc_codec, sizeof(g_media_sbc_codec),
        g_media_sbc_codec, sizeof(g_media_sbc_codec));
    if (!g_endpoint) return false;

    sdp_init();
    memset(g_sdp_a2dp, 0, sizeof(g_sdp_a2dp));
    a2dp_sink_create_sdp_record(g_sdp_a2dp, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_SPEAKER, nullptr, nullptr);
    sdp_register_service(g_sdp_a2dp);

    // Discoverable and connectable, or nothing can find it to pair with.
    gap_set_local_name(reg_get("BT.Name", "RPCortex"));
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200414);      // audio, loudspeaker
    gap_connectable_control(1);

    g_audio_pin = pin;
    log_addf(LOG_K_OK, "btaudio: sink ready on GPIO %d", pin);
    return true;
}

// --- the command ------------------------------------------------------------

static int cmd_btaudio(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        out_info("Bluetooth audio");
        out_multi("  Name       %s", reg_get("BT.Name", "RPCortex"));
        out_multi("  Sink       %s", g_audio_pin >= 0 ? "ready" : "not started");
        if (g_audio_pin >= 0) out_multi("  Output     PWM on GPIO %d", g_audio_pin);
        out_multi("  Connected  %s", g_connected ? g_peer : "nothing");
        out_multi("  Playing    %s", g_playing ? "yes" : "no");
        out_multi("  Format     %u Hz, %d channel%s",
                  (unsigned)g_sample_rate, g_channels, g_channels == 1 ? "" : "s");
        out_multi("  Volume     %u%%", (unsigned)g_volume);
        // Underruns are the number worth watching: they mean the decoder is not
        // keeping the ring fed, which is what a stutter sounds like.
        out_multi("  Underruns  %u", (unsigned)g_underruns);
        return 0;
    }

    if (!strcmp(sub, "start")) {
        int pin = argc > 2 ? atoi(argv[2]) : -1;
        if (pin < 0) { out_multi("Usage: btaudio start <gpio>"); return 1; }
        if (!fw_gpio_usable((unsigned)pin)) {
            out_err("GPIO %d cannot be used for this.", pin);
            return 1;
        }
        net_op_acquire();
        bool ok = net_core_ok() && btaudio_start(pin);
        net_op_release();
        if (!ok) { out_err("Could not start the audio sink."); return 1; }
        out_ok("Ready. Pair with '%s' from your phone.", reg_get("BT.Name", "RPCortex"));
        out_multi("  Audio comes out on GPIO %d. A speaker wants a resistor and", pin);
        out_multi("  a capacitor between the two; a powered amplifier does not.");
        return 0;
    }

    if (!strcmp(sub, "volume")) {
        if (argc < 3) { out_multi("Volume is %u%%.", (unsigned)g_volume); return 0; }
        int v = atoi(argv[2]);
        if (v < 0 || v > 100) { out_err("Volume is 0 to 100."); return 1; }
        g_volume = (uint32_t)v;
        out_ok("Volume %d%%.", v);
        return 0;
    }

    if (!strcmp(sub, "name")) {
        if (argc < 3) { out_multi("Name is '%s'.", reg_get("BT.Name", "RPCortex")); return 0; }
        reg_set("BT.Name", argv[2]);
        persist_save_dirty();
        out_ok("Name set to '%s'. It applies the next time the sink starts.", argv[2]);
        return 0;
    }

    if (!strcmp(sub, "stop")) {
        audio_stop();
        out_ok("Output stopped.");
        return 0;
    }

    out_multi("Usage:");
    out_multi("  btaudio start <gpio>    become a Bluetooth speaker on that pin");
    out_multi("  btaudio status          what is connected and playing");
    out_multi("  btaudio volume [0-100]");
    out_multi("  btaudio name [text]     what phones see when pairing");
    out_multi("  btaudio stop");
    return argc > 1 ? 1 : 0;
}

#else

static int cmd_btaudio(int, char **) {
    out_err("This board has no Bluetooth hardware.");
    return 1;
}

#endif

void btaudio_register(void) {
    static const Command c{"btaudio", "be a Bluetooth speaker", cmd_btaudio,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
