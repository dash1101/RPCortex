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

# test name -> extra sources
declare -A SRC=(
    [path_test]="$CORE/path.cpp"
    [httpparse_test]="$CORE/httpparse.cpp"
    [repoindex_test]="$CORE/repoindex.cpp"
    [preempt_test]="$CORE/preempt.cpp"
    [mpu_test]="$CORE/mpu.cpp"
    # The package source is INCLUDED by the test rather than linked: its scanners
    # are static, and they are the part that fails without saying so.
    [websearch_test]=""
    [httpd_test]=""
    [arena_test]="$CORE/arena.cpp"
    [fatview_test]="$CORE/fatview.cpp"
    [fatimage_test]="$CORE/fatview.cpp"
    [fat12_test]="$CORE/fat12.cpp $CORE/fatview.cpp"
    [blockcache_test]="$CORE/blockcache.cpp"
    [ptrcheck_test]="$CORE/ptrcheck.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [excframe_test]="$CORE/excframe.cpp"
    [tui_test]="$CORE/tui.cpp"
    [tuilist_test]="$CORE/tuilist.cpp $CORE/tui.cpp"
    [rps_test]="$CORE/rps.cpp"
    [tuikey_test]="$CORE/tuikey.cpp"
    [httpfetch_test]="$CORE/httpfetch.cpp $CORE/httpparse.cpp"
    [cmdline_test]="$CORE/cmdline.cpp"
    [lineedit_test]="$CORE/lineedit.cpp $CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [fmt_test]="$CORE/fmt.cpp"
    [blackbox_test]="$CORE/blackbox.cpp"
    [logring_test]="$CORE/logring.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [task_test]="$CORE/task.cpp $CORE/blackbox.cpp"
    [smp_test]="$CORE/task.cpp $CORE/blackbox.cpp"
    [calc_test]=""
    [pio_test]=""
    [wav_test]="$CORE/wav.cpp"
    [pcmring_test]="$CORE/pcmring.cpp"
    [btname_test]="$CORE/btname.cpp"
    [power_test]="$CORE/powerpolicy.cpp"
    [framebuf_test]="$CORE/framebuf.cpp"
    # The Nova D1 canvas is INCLUDED by its test, the same as the packages above:
    # it is a package source with no firmware behind it, which is exactly what
    # lets the drawing code be checked here at all.
    [novacanvas_test]=""
    [novacore_test]=""
    [packages_test]="fakehw.cpp"
    [lock_test]="$CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp"
    [interrupt_test]="$CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [pkgindex_test]="$CORE/pkgindex.cpp"
    [joblist_test]="$CORE/joblist.cpp"
    [textcore_test]="$CORE/textcore.cpp $CORE/interrupt.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [history_test]="$CORE/history.cpp"
    [core_test]="$CORE/sha256.cpp $CORE/registry.cpp $CORE/users.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp"
    [out_test]="$CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp"
    [realapp_test]="$SPIKE/loader.cpp $CORE/mpu.cpp"
    [os_test]="$SHELL_DIR/command.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp host_task_stub.cpp $SPIKE/loader.cpp"
    [apps_test]="$CORE/mpu.cpp $CORE/arena.cpp host_sandbox_stub.cpp $SHELL_DIR/command.cpp $SHELL_DIR/apps.cpp $CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/blackbox.cpp $CORE/logring.cpp host_task_stub.cpp $SPIKE/loader.cpp"
)

# Extra flags for tests that need them. smp_test runs two real threads.
declare -A FLAGS=( [smp_test]="-pthread" )

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

pass=0; fail=0
for t in "${!SRC[@]}"; do
    [ -f "$t.cpp" ] || continue
    printf '  %-16s ' "$t"
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

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
