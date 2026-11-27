#!/usr/bin/env bash
# Host test runner for the v2 core.
#
# Everything under os/core is deliberately free of hardware headers so it can be
# compiled and run on the host — that is the only way this OS gets tested at all
# when no board is attached. Each test names the sources it needs below; there is
# no build system here on purpose, because a test you can run with one script is
# a test that actually gets run.

set -u
cd "$(dirname "$0")"

CORE=../core
SHELL_DIR=../shell
SPIKE=../../loader-spike/firmware
INC="-I.. -I$CORE -I$SHELL_DIR -I../include -I../kernel -I$SPIKE"
CXX="${CXX:-g++} -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Make UndefinedBehaviorSanitizer ABORT on the first diagnostic instead of its
# default, which is to print the line and keep going. Without this a misaligned
# load or a signed overflow logs a "runtime error" to stderr and the test still
# prints "ok" and exits 0 — the defect is reported to nobody and fails nothing.
# A test that cannot fail is not a test; this is what makes the sanitizer runs
# gate the build. (ASan already aborts by default.)
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:${UBSAN_OPTIONS:-}"

# test name -> extra sources
declare -A SRC=(
    [path_test]="$CORE/path.cpp"
    [imagecheck_test]="$CORE/imagecheck.cpp"
    [httpparse_test]="$CORE/httpparse.cpp"
    [repoindex_test]="$CORE/repoindex.cpp"
    [preempt_test]="$CORE/preempt.cpp"
    [joinguard_test]="$CORE/joinguard.cpp"
    [mpu_test]="$CORE/mpu.cpp"
    # The package source is INCLUDED by the test rather than linked: its scanners
    # are static, and they are the part that fails without saying so.
    [websearch_test]=""
    [browse_test]=""
    [ablayout_test]=""
    [relsig_test]="$CORE/relsig.cpp $CORE/ed25519.cpp $CORE/sha512.cpp $CORE/b64.cpp $CORE/sha256.cpp"
    [httpd_test]=""
    # Ported from v1: the same three, each with its package source included and
    # a fake underneath it — a filesystem in a map, a screen of cells, a device
    # that answers about itself.
    [backup_test]=""
    [fileexp_test]=""
    [editor_test]=""
    [sysmon_test]=""
    [arena_test]="$CORE/arena.cpp"
    [fatview_test]="$CORE/fatview.cpp"
    [fatimage_test]="$CORE/fatview.cpp"
    [fat12_test]="$CORE/fat12.cpp $CORE/fatview.cpp"
    # The memory card, in its two host-testable halves: the filesystem on it,
    # against volumes fsck.fat is asked to approve of, and the command sequence
    # that brings a card up, against a fake card written from the specification.
    # What is left needs a card and is marked DEVICE-UNCONFIRMED in sdcard_rp2.cpp.
    [fatro_test]="$CORE/fatro.cpp"
    [sdproto_test]="$CORE/sdproto.cpp"
    [blockcache_test]="$CORE/blockcache.cpp"
    [ptrcheck_test]="$CORE/ptrcheck.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [excframe_test]="$CORE/excframe.cpp"
    [tui_test]="$CORE/tui.cpp"
    [tuilist_test]="$CORE/tuilist.cpp $CORE/tui.cpp"
    [rps_test]="$CORE/rps.cpp"
    [b64_test]="$CORE/b64.cpp"
    [linkreq_test]="$CORE/linkreq.cpp"
    [tuikey_test]="$CORE/tuikey.cpp"
    [httpfetch_test]="$CORE/httpfetch.cpp $CORE/httpparse.cpp"
    # The network bridge (LINK.md protocol 3): the device's transport driven
    # through http_fetch against a fake host, and the console's input door.
    [bridge_test]="$CORE/bridge.cpp $CORE/b64.cpp $CORE/httpfetch.cpp $CORE/httpparse.cpp"
    [cmdline_test]="$CORE/cmdline.cpp"
    [lineedit_test]="$CORE/lineedit.cpp $CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [fmt_test]="$CORE/fmt.cpp"
    [blackbox_test]="$CORE/blackbox.cpp"
    [logring_test]="$CORE/logring.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [crashlog_test]="$CORE/crashlog.cpp $CORE/logring.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [idle_test]="$CORE/idle.cpp"
    [clockcfg_test]="$CORE/clockcfg.cpp"
    # Mutation fuzzing of the parsers that read other people's bytes: HTTP
    # responses, package indexes, the installed-package list and FAT volumes.
    # RPC_FUZZ_ITERS=2000000 for a long run; see the header of fuzz_test.cpp.
    # The same for the package loader, which reads ELF objects somebody else built.
    # Needs the packages build.sh produces as its seeds, like realapp_test.
    [fuzz_loader_test]="$SPIKE/loader.cpp $SPIKE/pkgslot.cpp"
    [fuzz_test]="$CORE/httpparse.cpp $CORE/repoindex.cpp $CORE/pkgindex.cpp $CORE/fatro.cpp $CORE/cmdline.cpp $CORE/lineedit.cpp $CORE/clockcfg.cpp $CORE/linkreq.cpp $CORE/b64.cpp $CORE/path.cpp $CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [secret_test]="$CORE/secret.cpp $CORE/sha256.cpp"
    [protect_test]="$CORE/protect.cpp $CORE/path.cpp"
    [fsguard_test]="$SHELL_DIR/fsguard.cpp $CORE/ident.cpp $CORE/protect.cpp $CORE/path.cpp $CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp"
    [task_test]="$CORE/task.cpp $CORE/blackbox.cpp"
    [smp_test]="$CORE/task.cpp $CORE/blackbox.cpp"
    [calc_test]=""
    [pio_test]=""
    [wav_test]="$CORE/wav.cpp"
    [pcmring_test]="$CORE/pcmring.cpp"
    [btname_test]="$CORE/btname.cpp"
    # The advertising payload builders (from shell/bt.cpp via BT_ADV_BUILDER_ONLY)
    # and a render check for the Ping screen, which is reached by a gesture and so
    # is the one screen novashots cannot photograph on its own.
    [btadv_test]=""
    # The two contact readers. Their bus transactions need a chip on the end and
    # cannot be run here at all; the frames and the checksum those transactions
    # carry are arithmetic, and every way one can be wrong is reachable.
    [nfcframe_test]="$CORE/nfcframe.cpp"
    # The per-radio scan list, which the shell's name-merged one cannot stand in for.
    [scanlist_test]="$CORE/scanlist.cpp"
    # An update streamed straight into the staging area, against a model flash.
    [stagesink_test]="$CORE/stagesink.cpp $CORE/sha256.cpp"
    # Whether a stopped service is started again, and when.
    [svcpolicy_test]="$CORE/svcpolicy.cpp"
    [onewire_test]="$CORE/onewire.cpp"
    # The two SPI radios: frequency->register maths, band limits, LoRa modem-config
    # packing and its LDRO threshold, hex/timing parsing, and the exact shell text
    # the mesh agent and the Nova D1 screens parse. Pure sections of shell/cc1101,
    # shell/sx1276 and apps/novad1/novagui_radios are INCLUDED via RADIO_HOST_TEST /
    # RADIO_PARSE_ONLY, the same as btadv_test does with bt.cpp.
    [radio_test]=""
    [power_test]="$CORE/powerpolicy.cpp"
    [framebuf_test]="$CORE/framebuf.cpp"
    # The Nova D1 canvas is INCLUDED by its test, the same as the packages above:
    # it is a package source with no firmware behind it, which is exactly what
    # lets the drawing code be checked here at all.
    [novacanvas_test]=""
    [novacore_test]=""
    # Third-party apps, with a filesystem the test rewrites between cases: most
    # of what can go wrong with an app is a directory whose contents are wrong.
    [novaapps_test]=""
    # The runner, with the hardware faked. Catches what is logic rather than
    # electrons: screen lifetimes, a stack pushed past its pool, navigation that
    # traps, and a runner started twice — which is what took a board down.
    [novagui_test]=""
    # Not a test — it renders the UI to a page. Built here so it cannot rot
    # unnoticed: a change to the canvas that stops it compiling is a change
    # somebody wants to know about on the same run.
    [novasim]=""
    # Opens every screen the app catalogue can reach and looks at the panel it
    # produced. Fails on a screen that draws nothing — which on the device is
    # indistinguishable from a hang — and on one painting into the status bar's
    # rows, which the runner overwrites every frame.
    #
    # Deliberately not a pixel comparison against stored frames: every one of
    # these screens is meant to change, and a test that fails when a label is
    # reworded is a test somebody turns off. Run it with no arguments to get the
    # frames as characters and actually look at a layout.
    [novashots]=""
    # The simulator the website runs, built natively and driven through the same
    # exported entry points the page uses (sim_init / sim_key / sim_frame): it boots,
    # every app opens by key and draws something, and no worker is left queued.
    [sim_wasm_test]=""
    # The .sub / .ir / .nfc codecs, held byte for byte to what the MicroPython modules
    # that wrote those files first answered. codec_vectors.inc is generated from them
    # (gen_codec_vectors.py) and committed, so this needs only a compiler.
    [novacodec_test]=""
    # The USB personality stack, in its three pure pieces: the ASCII->keycode
    # map (a wrong entry types the wrong letter with no error), the DuckyScript
    # reader (a typo must never become keystrokes), and the mode arithmetic that
    # keeps the drive and the keyboard from ever being present at once. All three
    # are hardware-free by design so the safety rules are provable off-device;
    # usbmode.h and hidkey.h are header-only, ducky.cpp is a real source.
    [hidkey_test]=""
    [ducky_test]="$CORE/ducky.cpp"
    [usbmode_test]=""
    [packages_test]="fakehw.cpp"
    [lock_test]="$CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp"
    [interrupt_test]="$CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [pkgindex_test]="$CORE/pkgindex.cpp"
    [joblist_test]="$CORE/joblist.cpp"
    [detach_test]="$CORE/detach.cpp"
    [textcore_test]="$CORE/textcore.cpp $CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [history_test]="$CORE/history.cpp"
    [core_test]="$CORE/sha256.cpp $CORE/registry.cpp $CORE/users.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    # Saving the registry while another task changes it: persist.cpp is included.
    [persist_test]="$CORE/sha256.cpp $CORE/registry.cpp $CORE/users.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [out_test]="$CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp"
    # host_describe.cpp is what makes the region checks bind to the real thing:
    # it includes shell/apps.cpp so realapp_test can call describe() and compare
    # its own shadow of the protection map against it. Everything after it is
    # what apps.cpp needs to link, and nothing this test calls.
    [realapp_test]="$SPIKE/loader.cpp $CORE/mpu.cpp $SPIKE/pkgslot.cpp host_describe.cpp $CORE/arena.cpp host_sandbox_stub.cpp $SHELL_DIR/command.cpp $CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp"
    # The flash slot a package runs from: the format, and the write order that
    # makes an interrupted install safe. Proved against a fake chip, because the
    # interesting cases all involve losing power at a chosen moment.
    [pkgslot_test]="$SPIKE/pkgslot.cpp $SPIKE/loader.cpp"
    [os_test]="$SHELL_DIR/command.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp $SPIKE/loader.cpp"
    [apps_test]="$CORE/mpu.cpp $CORE/arena.cpp $CORE/detach.cpp host_sandbox_stub.cpp $SHELL_DIR/command.cpp $SHELL_DIR/apps.cpp $CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp $SPIKE/loader.cpp"
)

# Extra flags for tests that need them. smp_test runs two real threads.
declare -A FLAGS=( [smp_test]="-pthread" )

# --- SSH: the suites that need mbedTLS ---------------------------------------
#
# Built from the SDK's mbedTLS sources with the DEVICE's configuration, as
# cacerts_test.sh does, so a primitive the device does not have cannot pass
# here. Compiled once per run into $OUT, in parallel (about two seconds), and
# under the same sanitizers as everything else. Without an SDK checkout the
# SSH suites say SKIPPED rather than failing to build.
SDK_DIR="${PICO_SDK_PATH:-$PWD/../../sdk}"
MBED_OK=0
if [ -d "$SDK_DIR/lib/mbedtls/library" ]; then
    mkdir -p "$OUT/mbed"
    MBED_CFG="-DMBEDTLS_CONFIG_FILE=\"$PWD/../mbedtls_config.h\""
    mbed_one() {
        gcc -c -O1 -g -w -fsanitize=address,undefined -DRPC_HAS_SSH=1 "$MBED_CFG" \
            -I "$SDK_DIR/lib/mbedtls/include" -o "$OUT/mbed/$(basename "$1" .c).o" "$1"
    }
    export -f mbed_one; export MBED_CFG SDK_DIR OUT
    if ls "$SDK_DIR"/lib/mbedtls/library/*.c | xargs -P "$(nproc)" -I{} bash -c 'mbed_one "$1"' _ {} \
            2>"$OUT/mbed.err" && ar rcs "$OUT/libmbed.a" "$OUT"/mbed/*.o; then
        MBED_OK=1
    fi
fi
SSH_FLAGS="-DRPC_HAS_SSH=1 $MBED_CFG -I$SDK_DIR/lib/mbedtls/include"
SSH_CORE="$CORE/sshwire.cpp $CORE/sshcrypto.cpp $CORE/ed25519.cpp $CORE/sha512.cpp $CORE/sshtrans.cpp $CORE/sshserver.cpp $CORE/sshclient.cpp $CORE/sshkeys.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
# The protocol core against published vectors and a misbehaving peer.
SRC[ssh_test]="$SSH_CORE $CORE/sshsess.cpp $CORE/sha256.cpp $OUT/libmbed.a"
FLAGS[ssh_test]="$SSH_FLAGS"
# Mutation fuzzing of everything a peer can send, before and after login.
SRC[ssh_fuzz_test]="$SSH_CORE $OUT/libmbed.a"
FLAGS[ssh_fuzz_test]="$SSH_FLAGS"
# Ed25519 verification on its own: RFC 8032's vectors and its strict rules.
SRC[ed25519_test]="$CORE/ed25519.cpp $CORE/sha512.cpp $OUT/libmbed.a"
FLAGS[ed25519_test]="$SSH_FLAGS"
NEEDS_MBED="ssh_test ssh_fuzz_test ed25519_test"
# Who a command runs as: a remote login carries its own user, so a guest logged
# in over SSH is a guest whoever is at the console, and the other way round.
# The real ident, users, perms, fsguard, protect and interrupt code, on real
# task switches (its own ucontext seam, like task_test).
SRC[identity_test]="$SHELL_DIR/fsguard.cpp $CORE/ident.cpp $CORE/users.cpp $CORE/sha256.cpp $CORE/perms.cpp $CORE/protect.cpp $CORE/path.cpp $CORE/out.cpp $CORE/logring.cpp $CORE/interrupt.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp"
# What each command needs (user, admin, or sudo), the commands whose level
# depends on their arguments, the settings that decide who gets in, and that
# each rule is attached to its command in the shell's own sources.
SRC[perms_test]="$CORE/perms.cpp $CORE/users.cpp $CORE/sha256.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
# sudo end to end: the gate, the password, the grace, who can be asked (the
# console, a background job, a service, SSH with and without a pty, link's
# AUTH), the maintenance boot, and the file guard's line between a user's
# command and the OS's own writes. Real tasks on ucontext, like identity_test.
SRC[sudo_test]="$CORE/sudo.cpp $CORE/perms.cpp $SHELL_DIR/fsguard.cpp $CORE/ident.cpp $CORE/users.cpp $CORE/sha256.cpp $CORE/protect.cpp $CORE/path.cpp $CORE/out.cpp $CORE/logring.cpp $CORE/interrupt.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/registry.cpp $CORE/persist.cpp"

# --- the radio firmware, stored deflated ---------------------------------------
#
# fwstream_test reads the deflated CYW43 firmware back the way the patched
# driver does and compares it with the SDK's own copy, so it needs that copy and
# a stream made from it by the generator the build runs. uzlib is C and is
# compiled as C here; g++ would take it for C++ and refuse it. Without an SDK
# checkout the suite says SKIPPED. With one, a generator that fails is a failure.
CYW43_FWDIR="$SDK_DIR/lib/cyw43-driver/firmware"
FW_OK=0
if [ -f "$CYW43_FWDIR/wb43439A0_7_95_49_00_combined.h" ]; then
    mkdir -p "$OUT/fw"
    FW_CC="gcc -c -std=c99 -g -fsanitize=address,undefined -DUZLIB_CONF_PARANOID_CHECKS=1 -I../uzlib -I$OUT/fw"
    if python3 ../tools_cyw43fw.py --pad 4 "$CYW43_FWDIR/wb43439A0_7_95_49_00_combined.h" \
            "$OUT/fw/cyw43_fw_data" > "$OUT/fw/gen.out" 2>&1 \
        && $FW_CC ../uzlib/tinflate.c -o "$OUT/fw/tinflate.o" 2>>"$OUT/fw/gen.out" \
        && $FW_CC ../uzlib/crc32.c -o "$OUT/fw/crc32.o" 2>>"$OUT/fw/gen.out" \
        && $FW_CC ../uzlib/adler32.c -o "$OUT/fw/adler32.o" 2>>"$OUT/fw/gen.out" \
        && $FW_CC "$OUT/fw/cyw43_fw_data.c" -o "$OUT/fw/cyw43_fw_data.o" 2>>"$OUT/fw/gen.out"; then
        FW_OK=1
    else
        FW_OK=2
    fi
fi
SRC[fwstream_test]="$CORE/fwstream.cpp $OUT/fw/tinflate.o $OUT/fw/crc32.o $OUT/fw/adler32.o $OUT/fw/cyw43_fw_data.o"
FLAGS[fwstream_test]="-I../uzlib -I$OUT/fw -I$CYW43_FWDIR"

# Tests worth running a SECOND time with no sanitizer.
#
# AddressSanitizer replaces malloc with its own allocator, and that allocator
# aligns every block generously — so code that over-allocates and aligns inside
# what it got can free the wrong pointer and ASan will never notice, because the
# two pointers are equal under it. Real glibc hands back something 16-byte
# aligned about half the time, and freeing a pointer it did not issue aborts on
# the spot. task_spawn does exactly that alignment for the stack guard, so the
# uninstrumented run is the one that can catch it.
PLAIN="task_test ptrcheck_test"

pass=0; fail=0; skip=0
for t in "${!SRC[@]}"; do
    [ -f "$t.cpp" ] || continue
    printf '  %-16s ' "$t"
    if [ "$MBED_OK" -eq 0 ] && [[ " $NEEDS_MBED " == *" $t "* ]]; then
        echo "SKIPPED (no mbedTLS at $SDK_DIR/lib/mbedtls)"
        skip=$((skip+1)); continue
    fi
    if [ "$t" = fwstream_test ] && [ "$FW_OK" -ne 1 ]; then
        if [ "$FW_OK" -eq 0 ]; then
            echo "SKIPPED (no cyw43-driver firmware at $CYW43_FWDIR)"
            skip=$((skip+1))
        else
            echo "FAILED (making the stream)"; sed 's/^/      /' "$OUT/fw/gen.out" | tail -8
            fail=$((fail+1))
        fi
        continue
    fi
    if ! $CXX $INC ${FLAGS[$t]:-} "$t.cpp" ${SRC[$t]} -o "$OUT/$t" 2>"$OUT/$t.err"; then
        echo "BUILD FAILED"; sed 's/^/      /' "$OUT/$t.err" | head -15
        fail=$((fail+1)); continue
    fi
    if "$OUT/$t" >"$OUT/$t.out" 2>&1; then
        echo "ok   $(grep -oE '[0-9]+ checks?' "$OUT/$t.out" | tail -1)"
        pass=$((pass+1))
    else
        echo "FAILED"; sed 's/^/      /' "$OUT/$t.out" | tail -20
        fail=$((fail+1))
    fi
done

for t in $PLAIN; do
    printf '  %-16s ' "$t (plain)"
    if ! ${CXX%% -std*} -std=c++17 -w $INC ${FLAGS[$t]:-} "$t.cpp" ${SRC[$t]} \
            -o "$OUT/$t.plain" 2>"$OUT/$t.perr"; then
        echo "BUILD FAILED"; sed 's/^/      /' "$OUT/$t.perr" | head -10
        fail=$((fail+1)); continue
    fi
    if "$OUT/$t.plain" >"$OUT/$t.pout" 2>&1; then
        echo "ok   real allocator"
        pass=$((pass+1))
    else
        echo "FAILED"; sed 's/^/      /' "$OUT/$t.pout" | tail -12
        fail=$((fail+1))
    fi
done

# The CA bundle is not a C++ test — it needs mbedtls compiled with the DEVICE
# config, since the host's own build would parse a bundle the device cannot.
printf '  %-16s ' "cacerts"
if PICO_SDK_PATH="${PICO_SDK_PATH:-$PWD/../../sdk}" ./cacerts_test.sh > "$OUT/cacerts.out" 2>&1; then
    echo "ok  $(tail -1 "$OUT/cacerts.out" | tr -s ' ')"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/cacerts.out" | tail -6
    fail=$((fail+1))
fi

# littlefs is not safe to enter from two tasks at once, and nothing at run time
# says when a call slipped outside the lock - it surfaces days later as a disk
# that reads full (PAR-63). So the rule is checked in the source.
printf '  %-16s ' "fslock"
if python3 ../../tools/check-fslock.py > "$OUT/fslock.out" 2>&1; then
    echo "ok"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/fslock.out" | tail -12
    fail=$((fail+1))
fi

# A package calls the firmware by INDEX into the ABI table, and the flash slot
# keeps a package across updates only while those indices hold. The table grows
# at the end and nowhere else; a symbol inserted in the middle cost Nova D1 its
# slot (and the board most of its memory) before this check existed.
printf '  %-16s ' "abi_order"
if python3 ../../tools/check-abi-order.py ../.. > "$OUT/abi_order.out" 2>&1; then
    echo "ok"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/abi_order.out" | tail -6
    fail=$((fail+1))
fi

# The release tooling decides what a release is called and what it says, so it is
# held to the same standard as the firmware.
printf '  %-16s ' "release_py"
if python3 ../../tools/test_release.py > "$OUT/release_py.out" 2>&1; then
    echo "ok  $(grep -o 'Ran [0-9]* tests' "$OUT/release_py.out")"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/release_py.out" | tail -8
    fail=$((fail+1))
fi

# Feature selection: what each board's standard image has, what --with and
# --without turn into for CMake, what a custom image is called, and the marker
# a release reads back out of an image. build.sh, os/CMakeLists.txt,
# tools/buildconf.py and tools/release.py all go through tools/features.py.
printf '  %-16s ' "features_py"
if python3 ../../tools/test_features.py > "$OUT/features_py.out" 2>&1; then
    echo "ok  $(grep -o 'Ran [0-9]* tests' "$OUT/features_py.out")"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/features_py.out" | tail -12
    fail=$((fail+1))
fi

# The computer's side, RPCortex Studio (rpc, rpc tui, the bridge's host end),
# against its mock devices, with this repository as its RPCORTEX_DIR so its
# tests also check it against the firmware's own files. Studio is its own
# repository: RPC_STUDIO_DIR names a checkout, by default one beside this
# repository. Without one it is not run, and counts as neither a pass nor a
# failure: a skipped test is not a passed one.
STUDIO="${RPC_STUDIO_DIR:-../../../rpcortex-studio}"
printf '  %-16s ' "rpc_tools"
if [ ! -d "$STUDIO/tests" ]; then
    echo "not run: no rpcortex-studio checkout at $STUDIO (set RPC_STUDIO_DIR)"
elif RPCORTEX_DIR="$(cd ../.. && pwd)" python3 -m unittest discover -s "$STUDIO/tests" > "$OUT/rpc_tools.out" 2>&1; then
    echo "ok  $(grep -o 'Ran [0-9]* tests' "$OUT/rpc_tools.out")"
    pass=$((pass+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/rpc_tools.out" | tail -12
    fail=$((fail+1))
fi

# Needs a JavaScript runtime. Without one it says so and counts as neither a
# pass nor a failure: a skipped test is not a passed one.
JSRUN=""
if command -v node > /dev/null 2>&1; then JSRUN="node"
elif command -v deno > /dev/null 2>&1; then JSRUN="deno run --allow-net=127.0.0.1"
fi
# Every *.test.js in tools/web/test (the protocol client, the version
# comparison, the update and package flows, the network bridge), each in its
# own process. The bridge's test serves its URLs from 127.0.0.1, which is all
# the network Deno is allowed.
printf '  %-16s ' "web_link_js"
if [ -z "$JSRUN" ]; then
    echo "not run: no node or deno on PATH"
else
    : > "$OUT/web_link_js.out"
    web_ok=1; web_n=0; web_pass=0
    for t in ../../tools/web/test/*.test.js; do
        web_n=$((web_n+1))
        echo "== $(basename "$t")" >> "$OUT/web_link_js.out"
        if $JSRUN "$t" >> "$OUT/web_link_js.out" 2>&1; then
            # "N passed, ..." when the file says so; any other last line counts nothing.
            n=$(tail -1 "$OUT/web_link_js.out" | awk '{print $1}')
            case "$n" in ''|*[!0-9]*) n=0 ;; esac
            web_pass=$((web_pass + n))
        else
            web_ok=0
        fi
    done
    if [ "$web_ok" = 1 ] && [ "$web_n" -gt 0 ]; then
        echo "ok  $web_pass passed in $web_n files"
        pass=$((pass+1))
    else
        echo "FAILED"; grep -B1 -A3 "FAIL\|error" "$OUT/web_link_js.out" | sed 's/^/      /' | tail -16
        fail=$((fail+1))
    fi
fi

# SSH against the system's real OpenSSH, client and (when installed) sshd.
# Exit 77 is "could not run here": no ssh, no mbedTLS. That is counted as
# skipped and said so, never as passed.
printf '  %-16s ' "ssh_interop"
PICO_SDK_PATH="$SDK_DIR" ./ssh_interop_test.sh > "$OUT/ssh_interop.out" 2>&1
rc=$?
if [ $rc -eq 0 ]; then
    echo "ok   $(grep -oE '[0-9]+ checks' "$OUT/ssh_interop.out" | tail -1)"
    grep '^  (' "$OUT/ssh_interop.out" | sed 's/^/    /'
    pass=$((pass+1))
elif [ $rc -eq 77 ]; then
    echo "SKIPPED $(grep -o 'SKIPPED: .*' "$OUT/ssh_interop.out" | head -1 | cut -c10-)"
    skip=$((skip+1))
else
    echo "FAILED"; sed 's/^/      /' "$OUT/ssh_interop.out" | tail -20
    fail=$((fail+1))
fi

echo
skipped=""
[ "$skip" -gt 0 ] && skipped=", $skip skipped"
echo "  $pass passed, $fail failed$skipped"
[ "$fail" -eq 0 ]
