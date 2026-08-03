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
    [cmdline_test]="$CORE/cmdline.cpp"
    [lineedit_test]="$CORE/lineedit.cpp $CORE/interrupt.cpp $CORE/task.cpp host_task_stub.cpp"
    [fmt_test]="$CORE/fmt.cpp"
    [logring_test]="$CORE/logring.cpp $CORE/task.cpp host_task_stub.cpp"
    [task_test]="$CORE/task.cpp"
    [lock_test]="$CORE/lock.cpp $CORE/task.cpp"
    [interrupt_test]="$CORE/interrupt.cpp $CORE/task.cpp host_task_stub.cpp"
    [pkgindex_test]="$CORE/pkgindex.cpp"
    [joblist_test]="$CORE/joblist.cpp"
    [textcore_test]="$CORE/textcore.cpp $CORE/interrupt.cpp $CORE/task.cpp host_task_stub.cpp"
    [history_test]="$CORE/history.cpp"
    [core_test]="$CORE/sha256.cpp $CORE/registry.cpp $CORE/users.cpp $CORE/lock.cpp $CORE/task.cpp host_task_stub.cpp"
    [out_test]="$CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/logring.cpp host_task_stub.cpp"
    [os_test]="$SHELL_DIR/command.cpp $CORE/lock.cpp $CORE/task.cpp host_task_stub.cpp $SPIKE/loader.cpp"
    [apps_test]="$SHELL_DIR/command.cpp $SHELL_DIR/apps.cpp $CORE/out.cpp $CORE/lock.cpp $CORE/task.cpp $CORE/logring.cpp host_task_stub.cpp $SPIKE/loader.cpp"
)

pass=0; fail=0
for t in "${!SRC[@]}"; do
    [ -f "$t.cpp" ] || continue
    printf '  %-16s ' "$t"
    if ! $CXX $INC "$t.cpp" ${SRC[$t]} -o "$OUT/$t" 2>"$OUT/$t.err"; then
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

echo
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ]
