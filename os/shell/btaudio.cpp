// A2DP source — playing a file to a Bluetooth speaker.
//
// The thing v1 could not do at any level of effort. MicroPython's `bluetooth`
// module is BLE only, so Classic, and therefore audio, needed C or a custom
// firmware build.
//
// SOURCE, not sink: this device reads a file and sends it to headphones or a
// speaker, which is what a media player does. The other direction — the device
// BEING a speaker — would need a speaker wired to a pin, because a Pico has
// nothing that makes a sound. Sending has no such problem: the thing at the
// other end already has an amplifier in it.
//
// It lives in the OS rather than a package because A2DP is four layers deep —
// L2CAP, AVDTP, A2DP, then SBC — and exposing enough of btstack to build it
// outside would mean exposing most of btstack.
//
// The pipeline, and where each piece runs:
//
//   the file     WAV on littlefs, read a block at a time by a task
//   the ring     holds PCM waiting to be sent; the only thing shared across
//                contexts
//   the encoder  pulls from the ring and makes SBC frames, in the radio's
//                interrupt, whenever btstack asks for more
//   the speaker  does the rest
//
// The ring is the design, and it is the same ring as before with the direction
// reversed. A file read takes as long as flash takes, and the radio asks for
// frames on its own schedule, so something has to absorb the difference without
// a lock — one side is an interrupt, and taking a lock there stalls the link.
//
// DEVICE-UNCONFIRMED. It builds, and the ring and the WAV parsing are
// host-tested; no speaker has been connected to it.

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "persist.h"
#include "pcmring.h"
#include "wav.h"
#include "storage.h"
#include "loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack.h"

void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);
bool bt_stack_up(void);

// --- the ring ---------------------------------------------------------------
//
// About a fifth of a second of stereo at 44.1 kHz. Long enough that a slow
// flash read does not interrupt the music, short enough that stopping does not
// leave a fifth of a second still to play.
#define PCM_RING_SAMPLES 8192
static int16_t g_pcm[PCM_RING_SAMPLES];
static PcmRing g_ring;

// --- what is playing --------------------------------------------------------

static btstack_sbc_encoder_bluedroid_t g_sbc_state;
static const btstack_sbc_encoder_t    *g_sbc;

static bool     g_connected;
static bool     g_streaming;
static char     g_peer[24];
static char     g_track[64];
static bd_addr_t g_target;
static uint16_t g_a2dp_cid;
static uint8_t  g_seid;
static WavInfo  g_wav;
static AppSource g_src;
static void     *g_file;
static uint32_t g_pos;              // bytes of audio sent so far
static uint32_t g_underruns;
static uint32_t g_volume = 80;

static uint8_t g_sdp_a2dp[150];
// What this device tells a speaker it can send.
//
// Four bytes, and all-zero means "nothing", which is what these were: a
// zero-filled array advertises no sample rate and no channel mode, and a
// speaker offered that has nothing to accept. The stream would have been
// refused before a single sample moved.
//
//   [0]  sample rates in the top nibble, channel modes in the bottom
//   [1]  block length, subbands, bit allocation
//   [2]  minimum bitpool
//   [3]  maximum bitpool — quality against bandwidth
static uint8_t g_media_codec[4] = {
    // 44.1 kHz and 48 kHz; mono, dual, stereo and joint stereo.
    (0x20 | 0x10) | (0x08 | 0x04 | 0x02 | 0x01),
    // Every block length and both subband counts, both allocation methods.
    // Offering everything lets the speaker pick, which is the point of the
    // negotiation — narrowing it here would only rule out speakers.
    0xFF,
    2,      // minimum bitpool the specification allows
    53,     // the usual maximum for 44.1 kHz stereo
};
static avdtp_stream_endpoint_t *g_endpoint;

// --- feeding the ring -------------------------------------------------------
//
// A task, not an interrupt: reading flash takes as long as it takes, and doing
// it in the radio's callback would stall the link for the duration. The ring is
// what lets the two run at their own speeds.

static int feeder_task(void *) {
    uint8_t block[1024];
    while (g_streaming && !task_should_stop()) {
        // Only top up when there is room for a whole block. Writing a partial
        // one would mean tracking a partial sample across reads, and a sample
        // split across two reads is a click.
        if (pcm_ring_free(&g_ring) < sizeof(block) / 2) {
            task_sleep_ms(5);
            continue;
        }

        uint32_t want = sizeof(block);
        uint32_t left = g_wav.data_bytes > g_pos ? g_wav.data_bytes - g_pos : 0;
        if (left == 0) break;                       // end of the file
        if (want > left) want = left;

        int got = g_src.read(g_src.ctx, g_wav.data_offset + g_pos, block, want);
        if (got <= 0) break;
        g_pos += (uint32_t)got;

        // The file holds little-endian 16-bit samples, which is what the ring
        // and the encoder both want, so this is a copy rather than a conversion.
        pcm_ring_write(&g_ring, (const int16_t *)block, (uint32_t)got / 2);
    }

    // Let what is already buffered finish rather than cutting it off mid-word.
    for (int i = 0; i < 100 && pcm_ring_used(&g_ring) > 0; i++) task_sleep_ms(10);

    if (g_streaming) {
        a2dp_source_pause_stream(g_a2dp_cid, g_seid);
        g_streaming = false;
        log_add(LOG_K_INFO, "btaudio: track finished");
    }
    if (g_file) { storage_close_source(g_file); g_file = nullptr; }
    return 0;
}

// --- sending ----------------------------------------------------------------
//
// btstack asks for frames when the link has room. Runs in the radio's
// interrupt, so it does the least it can: take PCM from the ring, encode, hand
// it over.

static void fill_frames(uint8_t seid, int frames_wanted) {
    uint8_t  media[512];
    int      pos = 1;                       // byte 0 is the frame count
    int      made = 0;

    unsigned samples_per_frame = (unsigned)g_sbc->num_audio_frames(&g_sbc_state);
    unsigned per_frame = samples_per_frame * g_wav.channels;

    for (int i = 0; i < frames_wanted; i++) {
        int16_t pcm[256 * 2];
        if (per_frame > sizeof(pcm) / sizeof(pcm[0])) break;
        if (pcm_ring_read(&g_ring, pcm, per_frame) != per_frame) {
            // Nothing ready. Sending a short packet is better than sending a
            // stale one, and the count byte below says how many are real.
            g_underruns++;
            break;
        }
        if (g_volume != 100)
            for (unsigned k = 0; k < per_frame; k++)
                pcm[k] = (int16_t)((int32_t)pcm[k] * (int32_t)g_volume / 100);

        // The encoder writes exactly sbc_buffer_length bytes and does not take
        // a capacity, so the room has to be checked before the call rather than
        // relied on afterwards.
        uint16_t frame_len = g_sbc->sbc_buffer_length(&g_sbc_state);
        if (pos + frame_len > (int)sizeof(media)) break;
        g_sbc->encode_signed_16(&g_sbc_state, pcm, media + pos);
        pos += frame_len;
        made++;
    }

    if (!made) return;
    media[0] = (uint8_t)made;
    // marker 0, timestamp counted in samples — the receiver uses it to pace
    // playback, so it advances by what was actually sent rather than by frames.
    static uint32_t ts;
    a2dp_source_stream_send_media_payload_rtp(g_a2dp_cid, seid, 0, ts, media, (uint16_t)pos);
    ts += (uint32_t)made * samples_per_frame;
}

static void a2dp_handler(uint8_t type, uint16_t, uint8_t *packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            bd_addr_t addr;
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            g_a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            g_connected = true;
            snprintf(g_peer, sizeof(g_peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            log_addf(LOG_K_OK, "btaudio: connected to %s", g_peer);
            break;
        }
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            g_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            g_streaming = true;
            break;

        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            fill_frames(a2dp_subevent_streaming_can_send_media_packet_now_get_local_seid(packet),
                        1);
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
        case A2DP_SUBEVENT_STREAM_RELEASED:
            g_streaming = false;
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            g_connected = false;
            g_streaming = false;
            g_peer[0] = 0;
            log_add(LOG_K_INFO, "btaudio: disconnected");
            break;

        default:
            break;
    }
}

static bool source_ready(void) {
    static bool inited;
    if (inited) return true;
    if (!bt_stack_up()) return false;

    pcm_ring_init(&g_ring, g_pcm, PCM_RING_SAMPLES);

    // The encoder, configured before anything asks it for a frame. Without
    // this it has no sample rate, no channel mode and no bitpool, and what it
    // produces is not SBC.
    g_sbc = btstack_sbc_encoder_bluedroid_init_instance(&g_sbc_state);
    g_sbc->configure(&g_sbc_state, SBC_MODE_STANDARD,
                     16,                        // blocks
                     8,                         // subbands
                     SBC_ALLOCATION_METHOD_LOUDNESS,
                     44100,
                     53,                        // bitpool
                     SBC_CHANNEL_MODE_JOINT_STEREO);

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_handler);
    g_endpoint = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        g_media_codec, sizeof(g_media_codec),
        g_media_codec, sizeof(g_media_codec));
    if (!g_endpoint) return false;

    sdp_init();
    memset(g_sdp_a2dp, 0, sizeof(g_sdp_a2dp));
    a2dp_source_create_sdp_record(g_sdp_a2dp, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, nullptr, nullptr);
    sdp_register_service(g_sdp_a2dp);

    gap_set_local_name(reg_get("BT.Name", "RPCortex"));
    inited = true;
    return true;
}

// --- commands ---------------------------------------------------------------

static bool parse_addr(const char *s, bd_addr_t out) {
    unsigned v[6];
    // siscanf, not sscanf: the integer-only scanf. Plain sscanf links newlib's
    // float-capable engine (__ssvfscanf_r) and, through it, the multi-precision
    // strtod/mprec cluster — about 12 KB of flash for a format that is all %x.
    // The integer engine is already in the image (newlib's tzset uses it), so
    // this costs nothing. Do NOT "simplify" back to sscanf.
    if (siscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 255) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static int cmd_btaudio(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        out_info("Bluetooth audio");
        out_multi("  Name       %s", reg_get("BT.Name", "RPCortex"));
        out_multi("  Speaker    %s", g_connected ? g_peer : "not connected");
        out_multi("  Playing    %s", g_streaming ? g_track : "nothing");
        if (g_streaming && g_wav.data_bytes)
            out_multi("  Position   %u%%  (%u Hz, %u channel%s)",
                      (unsigned)((uint64_t)g_pos * 100 / g_wav.data_bytes),
                      (unsigned)g_wav.sample_rate, (unsigned)g_wav.channels,
                      g_wav.channels == 1 ? "" : "s");
        out_multi("  Volume     %u%%", (unsigned)g_volume);
        // Underruns mean the file is not being read fast enough to keep the
        // link fed, which is what a stutter sounds like.
        out_multi("  Underruns  %u", (unsigned)g_underruns);
        return 0;
    }

    if (!strcmp(sub, "connect")) {
        if (argc < 3) { out_multi("Usage: btaudio connect <address>"); return 1; }
        if (!parse_addr(argv[2], g_target)) {
            out_err("That is not an address. 'bt scan classic' lists them.");
            return 1;
        }
        net_op_acquire();
        bool ok = net_core_ok() && source_ready();
        if (ok) ok = (a2dp_source_establish_stream(g_target, &g_a2dp_cid) == ERROR_CODE_SUCCESS);
        net_op_release();
        if (!ok) { out_err("Could not connect."); return 1; }

        out_info("Connecting to %s...", argv[2]);
        for (int i = 0; i < 100 && !g_connected; i++) task_sleep_ms(100);
        if (!g_connected) { out_err("It did not answer."); return 1; }
        out_ok("Connected to %s.", g_peer);
        return 0;
    }

    if (!strcmp(sub, "play")) {
        if (argc < 3) { out_multi("Usage: btaudio play <file.wav>"); return 1; }
        if (!g_connected) {
            out_err("Nothing to play to.");
            out_multi("  'bt scan classic' to find a speaker, then 'btaudio connect <address>'.");
            return 1;
        }
        if (g_streaming) { out_err("Already playing. 'btaudio stop' first."); return 1; }

        // The header only. A WAV header is under a hundred bytes and reading
        // the whole file to find out whether it is playable would mean reading
        // a file that might not be.
        uint8_t head[256];
        AppSource src;
        void *h = nullptr;
        if (!storage_open_source(argv[2], &src, &h)) {
            out_err("No such file: %s", argv[2]);
            return 1;
        }
        int n = src.read(src.ctx, 0, head, sizeof(head));
        if (n < 0) n = 0;
        WavResult wr = wav_parse(head, (uint32_t)n, &g_wav);
        if (wr != WAV_OK) {
            storage_close_source(h);
            out_err("Cannot play %s: %s.", argv[2], wav_result_str(wr));
            return 1;
        }

        g_file = h;
        g_src  = src;
        g_pos = 0;
        g_underruns = 0;
        snprintf(g_track, sizeof(g_track), "%s", argv[2]);
        pcm_ring_clear(&g_ring);

        net_op_acquire();
        a2dp_source_start_stream(g_a2dp_cid, g_seid);
        net_op_release();

        if (task_spawn("btaudio", "(kernel)", feeder_task, nullptr,
                       TASK_STACK_DEF, AFFINITY_CORE0) < 0) {
            storage_close_source(h);
            g_file = nullptr;
            out_err("No room for the playback task.");
            return 1;
        }
        out_ok("Playing %s (%u Hz, %u channel%s).", argv[2],
               (unsigned)g_wav.sample_rate, (unsigned)g_wav.channels,
               g_wav.channels == 1 ? "" : "s");
        return 0;
    }

    if (!strcmp(sub, "stop")) {
        if (!g_streaming) { out_info("Nothing playing."); return 0; }
        g_streaming = false;
        net_op_acquire();
        a2dp_source_pause_stream(g_a2dp_cid, g_seid);
        net_op_release();
        out_ok("Stopped.");
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
        out_ok("Name set to '%s'.", argv[2]);
        return 0;
    }

    out_multi("Usage:");
    out_multi("  btaudio connect <address>   pair with a speaker or headphones");
    out_multi("  btaudio play <file.wav>     play a file to it");
    out_multi("  btaudio stop");
    out_multi("  btaudio status              what is connected and playing");
    out_multi("  btaudio volume [0-100]");
    out_multi("  btaudio name [text]         what this device calls itself");
    out_multi("  Find a speaker with 'bt scan classic'. 16-bit WAV only.");
    return argc > 1 ? 1 : 0;
}

#else

static int cmd_btaudio(int, char **) {
    out_err("This board has no Bluetooth hardware.");
    return 1;
}

#endif

void btaudio_register(void) {
    static const Command c{"btaudio", "play audio to a Bluetooth speaker", cmd_btaudio,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
