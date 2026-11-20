#!/usr/bin/env bash
# SSH against the real thing: the server core behind a loopback socket with
# the system's OpenSSH client on the other end, and the client core against
# both that server and a real, unprivileged sshd.
#
# Every algorithm the server offers is forced in turn with -o KexAlgorithms=
# and -o Ciphers= / -o MACs=, so each one is proven by a peer that did not come
# from this repository rather than by our own client agreeing with our own
# server. The unit-level checks are in ssh_test.cpp.
#
# Needs `ssh` and `ssh-keygen`. Without them it says SKIPPED and exits 77,
# which run_all.sh reports as skipped — never as passed. The sshd part is
# skipped on its own, with a message, when there is no sshd.
#
# Built under AddressSanitizer and UBSan like every other suite here.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OS="$HERE/.."
SDK="${PICO_SDK_PATH:-$OS/../sdk}"
CORE="$OS/core"

for tool in ssh ssh-keygen python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "  SKIPPED: '$tool' is not installed, so there is no real peer to test against"
        exit 77
    fi
done
if [ ! -d "$SDK/lib/mbedtls/library" ]; then
    echo "  SKIPPED: mbedtls is not checked out at $SDK/lib/mbedtls"
    exit 77
fi

W="$(mktemp -d)"
cleanup() {
    [ -f "$W/server.pid" ] && kill "$(cat "$W/server.pid")" 2>/dev/null
    [ -f "$W/sshd.pid" ] && kill "$(cat "$W/sshd.pid")" 2>/dev/null
    [ -n "${SSH_KEEP:-}" ] && { echo "kept $W"; return; }
    rm -rf "$W"
}
trap cleanup EXIT

export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:${UBSAN_OPTIONS:-}"
SAN="-fsanitize=address,undefined"

# mbedTLS, compiled with the DEVICE's configuration (as cacerts_test.sh does):
# the host's own mbedTLS would have algorithms enabled that the device does not.
mkdir -p "$W/mbed"
CFG="-DMBEDTLS_CONFIG_FILE=\"$OS/mbedtls_config.h\""
mbed_one() {
    gcc -c -O1 -g -w $SAN -DRPC_HAS_SSH=1 "$CFG" -I "$SDK/lib/mbedtls/include" \
        -o "$W/mbed/$(basename "$1" .c).o" "$1"
}
export -f mbed_one; export SAN CFG SDK W
ls "$SDK"/lib/mbedtls/library/*.c | xargs -P "$(nproc)" -I{} bash -c 'mbed_one "$1"' _ {} || exit 1
ar rcs "$W/libmbed.a" "$W"/mbed/*.o

CXXF=(-std=c++17 -Wall -Wextra -Wno-unused-parameter -g $SAN -pthread -DRPC_HAS_SSH=1 "$CFG"
      -I"$OS" -I"$CORE" -I"$OS/shell" -I"$OS/include" -I"$OS/kernel" -I"$SDK/lib/mbedtls/include")
COMMON=("$CORE/sshwire.cpp" "$CORE/sshcrypto.cpp" "$CORE/sshtrans.cpp" "$CORE/sshkeys.cpp"
        "$CORE/lock.cpp" "$CORE/task.cpp" "$CORE/blackbox.cpp" "$HERE/host_task_stub.cpp" "$W/libmbed.a")
g++ "${CXXF[@]}" "$HERE/ssh_host_server.cpp" "$CORE/sshserver.cpp" "$CORE/sshsess.cpp" \
    "$CORE/lineedit.cpp" "$CORE/interrupt.cpp" "${COMMON[@]}" -o "$W/server" || exit 1
g++ "${CXXF[@]}" "$HERE/ssh_host_client.cpp" "$CORE/sshclient.cpp" "${COMMON[@]}" -o "$W/client" || exit 1

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "    FAIL: $*"; }
check() {   # check <description> <command...>: passes when the command does
    local what="$1"; shift
    if "$@"; then ok; else bad "$what"; fi
}

# --- the server -----------------------------------------------------------------
cd "$W"
ssh-keygen -q -t ecdsa -b 256 -N '' -f id_ecdsa -C test
ssh-keygen -q -t rsa -b 3072 -N '' -f id_rsa -C test
ssh-keygen -q -t ecdsa -b 256 -N '' -f id_other -C stranger
cat id_ecdsa.pub id_rsa.pub > authorized_keys
# A line with options must NOT be honoured: the stranger's key is listed, but
# behind a command= restriction this server cannot enforce.
echo "command=\"/bin/false\" $(cat id_other.pub)" >> authorized_keys

PORT=""
for try in 1 2 3 4 5 6 7 8; do
    p=$((20000 + RANDOM % 30000))
    ./server "$p" authorized_keys hostkey.pub > server.out 2> server.err &
    echo $! > server.pid
    for _ in $(seq 50); do grep -q READY server.out 2>/dev/null && break; sleep 0.05; done
    if grep -q READY server.out 2>/dev/null; then PORT=$p; break; fi
    kill "$(cat server.pid)" 2>/dev/null; rm -f server.pid
done
[ -n "$PORT" ] || { echo "    FAIL: the host server would not start"; cat server.err; exit 1; }

S="-p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
   -o IdentitiesOnly=yes -o ConnectTimeout=10"
B="$S -o BatchMode=yes -i id_ecdsa"
run() { timeout 60 ssh "$@"; }

# The fingerprint the server prints is the one ssh-keygen computes.
fp_srv=$(sed -n 's/^FINGERPRINT //p' server.out)
fp_kg=$(ssh-keygen -lf hostkey.pub | awk '{print $2}')
check "host key fingerprint ($fp_srv vs $fp_kg)" test -n "$fp_srv" -a "$fp_srv" = "$fp_kg"

# Every kex, every cipher, and for the CTR ciphers every MAC.
for kex in curve25519-sha256 curve25519-sha256@libssh.org ecdh-sha2-nistp256; do
    for c in aes128-gcm@openssh.com aes256-gcm@openssh.com aes128-ctr aes256-ctr; do
        case $c in *ctr) macs="hmac-sha2-256-etm@openssh.com hmac-sha2-256" ;; *) macs="-" ;; esac
        for m in $macs; do
            mo=""; [ "$m" != "-" ] && mo="-o MACs=$m"
            out=$(run $B -o KexAlgorithms=$kex -o Ciphers=$c $mo tester@127.0.0.1 "echo $kex-$c" 2>&1)
            check "kex=$kex cipher=$c mac=$m: '$out'" test "$out" = "$kex-$c"
        done
    done
done

# Strict KEX is in force (the Terrapin countermeasure), and ext-info is sent.
v=$(run $B -vvv tester@127.0.0.1 true 2>&1)
check "strict KEX not agreed" grep -q "will use strict KEX ordering" <<<"$v"
check "no server-sig-algs" grep -q "extension server-sig-algs" <<<"$v"

# RSA user keys, with both SHA-2 signature algorithms.
for alg in rsa-sha2-512 rsa-sha2-256; do
    out=$(run $S -o BatchMode=yes -i id_rsa -o PubkeyAcceptedAlgorithms=$alg tester@127.0.0.1 "echo $alg" 2>&1)
    check "rsa key with $alg: '$out'" test "$out" = "$alg"
done

# exec: output and exit status.
run $B tester@127.0.0.1 'exit 7' >/dev/null 2>&1; rc=$?
check "exit status 7 came back as $rc" test $rc -eq 7
run $B tester@127.0.0.1 'false' >/dev/null 2>&1; rc=$?
check "false came back as $rc" test $rc -eq 1

pattern() {   # the bytes `big N` sends
    python3 -c "
import sys,hashlib
p='0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/'
n=int(sys.argv[1])
sys.stdout.write(hashlib.sha256(''.join('\n' if j%64==63 else p[j%64] for j in range(n)).encode()).hexdigest())" "$1"
}

# 100 KB: about fifty window adjustments' worth with OpenSSH's 2 MB window
# split into our 2 KB packets — and byte-exact.
got=$(run $B tester@127.0.0.1 'big 102400' | sha256sum | cut -d' ' -f1)
check "100 KB output hash" test "$got" = "$(pattern 102400)"

# A megabyte with re-keying forced every 16 KB: dozens of key exchanges in the
# middle of a stream, in both directions' sequence numbers.
v=$(run $B -vv -o RekeyLimit=16K tester@127.0.0.1 'big 1048576' 2>rekey.log | sha256sum | cut -d' ' -f1)
check "1 MB output hash across re-keys" test "$v" = "$(pattern 1048576)"
nk=$(grep -c "SSH2_MSG_KEXINIT sent" rekey.log)
check "re-keying happened ($nk KEXINITs)" test "$nk" -ge 10

# An interactive shell on a terminal, input piped in.
out=$(printf 'echo hello\nwhoami\nexit 3\n' | run $S -tt -o BatchMode=yes -i id_ecdsa tester@127.0.0.1 2>/dev/null); rc=$?
check "interactive: echo" grep -q "^hello" <<<"$(tr -d '\r' <<<"$out")"
check "interactive: whoami" grep -q "^tester" <<<"$(tr -d '\r' <<<"$out")"
check "interactive: exit status $rc" test $rc -eq 3
check "interactive: lines end CR LF" grep -q $'hello\r$' <<<"$out"

# Ctrl+C reaches the command that is running, and only that.
python3 - "$PORT" <<'PY' > ctrlc.out 2>&1
import subprocess, sys, time
p = subprocess.Popen(["ssh", "-tt", "-p", sys.argv[1], "-o", "StrictHostKeyChecking=no",
     "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "BatchMode=yes",
     "-o", "IdentitiesOnly=yes", "-i", "id_ecdsa", "tester@127.0.0.1"],
     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
t0 = time.time()
p.stdin.write(b"sleep 30\n"); p.stdin.flush()
time.sleep(1.5)
p.stdin.write(b"\x03"); p.stdin.flush()
time.sleep(0.5)
p.stdin.write(b"echo after\nexit 0\n"); p.stdin.flush()
out, _ = p.communicate(timeout=20)
print(out.decode(errors="replace").replace("\r", ""))
print("ELAPSED %.1f" % (time.time() - t0))
PY
check "Ctrl+C interrupted the command" grep -q "^interrupted" ctrlc.out
check "the session carried on after Ctrl+C" grep -q "^after" ctrlc.out
el=$(sed -n 's/^ELAPSED //p' ctrlc.out | cut -d. -f1)
check "Ctrl+C took effect promptly (${el}s)" test -n "$el" -a "${el:-99}" -lt 15

# Two sessions at once.
run $B tester@127.0.0.1 'sleep 2' > two.a 2>&1 &
pa=$!
run $B tester@127.0.0.1 'sleep 2' > two.b 2>&1 &
pb=$!
wait $pa; ra=$?; wait $pb; rb=$?
check "two concurrent sessions ($ra/$rb)" test $ra -eq 0 -a $rb -eq 0 -a "$(cat two.a two.b)" = "$(printf 'slept\nslept')"

# Refusals.
out=$(run $S -o BatchMode=yes -i id_other tester@127.0.0.1 'echo no' 2>&1); rc=$?
check "a key listed only behind options is refused ($rc)" test $rc -eq 255 -a -n "$(grep -i 'permission denied' <<<"$out")"
ssh-keygen -q -t ecdsa -N '' -f id_unknown
out=$(run $S -o BatchMode=yes -i id_unknown tester@127.0.0.1 'echo no' 2>&1); rc=$?
check "an unknown key is refused ($rc)" test $rc -eq 255
out=$(run $S -o BatchMode=yes -i id_ecdsa nosuchuser@127.0.0.1 'echo no' 2>&1); rc=$?
check "an unknown user is refused ($rc)" test $rc -eq 255

printf '#!/bin/sh\necho secret\n' > goodpw.sh; printf '#!/bin/sh\necho wrong\n' > badpw.sh
chmod +x goodpw.sh badpw.sh
PW="$S -o PreferredAuthentications=password -o PubkeyAuthentication=no"
out=$(SSH_ASKPASS="$W/goodpw.sh" SSH_ASKPASS_REQUIRE=force setsid -w timeout 60 ssh $PW tester@127.0.0.1 'echo pw-ok' </dev/null 2>&1)
check "password login: '$out'" test "$out" = "pw-ok"
t0=$(date +%s)
out=$(SSH_ASKPASS="$W/badpw.sh" SSH_ASKPASS_REQUIRE=force setsid -w timeout 60 ssh $PW -o NumberOfPasswordPrompts=2 tester@127.0.0.1 'echo no' </dev/null 2>&1); rc=$?
t1=$(date +%s)
check "a wrong password is refused ($rc)" test $rc -eq 255
check "wrong passwords are slowed down ($((t1 - t0))s for two)" test $((t1 - t0)) -ge 2
check "refusals are logged" grep -q "tester password refused" server.err

# Enough wrong passwords and the connection is dropped, not just refused.
out=$(SSH_ASKPASS="$W/badpw.sh" SSH_ASKPASS_REQUIRE=force setsid -w timeout 90 ssh $PW -o NumberOfPasswordPrompts=10 tester@127.0.0.1 'echo no' </dev/null 2>&1); rc=$?
check "six failures end the connection ($rc)" test $rc -eq 255 -a -n "$(grep -i 'too many\|disconnect\|closed' <<<"$out")"

# --- the client core ----------------------------------------------------------------
out=$(./client -p "$PORT" -P secret -k kh tester@127.0.0.1 'echo via-client' 2>client.err); rc=$?
check "client core -> server core: '$out' ($rc)" test "$out" = "via-client" -a $rc -eq 0
check "client recorded the host key" grep -q "new host recorded" client.err
./client -p "$PORT" -P secret -k kh tester@127.0.0.1 'exit 9' >/dev/null 2>client.err; rc=$?
check "client: exit status 9 ($rc)" test $rc -eq 9
check "client: known host matched" grep -q "known host" client.err
got=$(./client -p "$PORT" -P secret tester@127.0.0.1 'big 102400' 2>/dev/null | sha256sum | cut -d' ' -f1)
check "client: 100 KB output hash" test "$got" = "$(pattern 102400)"
out=$(printf 'echo hi\nexit 4\n' | ./client -p "$PORT" -P secret -t tester@127.0.0.1 2>/dev/null); rc=$?
check "client: interactive ($rc)" test $rc -eq 4 -a -n "$(grep '^hi' <<<"$(tr -d '\r' <<<"$out")")"
./client -p "$PORT" -P wrong tester@127.0.0.1 'echo no' >/dev/null 2>client.err; rc=$?
check "client: wrong password gives up ($rc)" test $rc -eq 255
# A changed host key must stop the connection before a password is sent.
sed -i 's/ AAAA[^ ]*/ AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBCENn0qk58hbP3ySLHZOB\/peRjveVdUEgFQfQ8CWjJcBEtXGQldAnfnO22oF8h+mi6D3AE9WtsnLF+MYddWoT0U=/' kh
./client -p "$PORT" -P secret -k kh tester@127.0.0.1 'echo no' >/dev/null 2>client.err; rc=$?
check "client: a changed host key is refused ($rc)" test $rc -eq 255 -a -n "$(grep 'HOST KEY CHANGED' client.err)"
check "client: no password asked of a changed host" test -z "$(grep 'prompt:' client.err)"

# --- the client core against a real sshd -------------------------------------------
SSHD=""
for c in /usr/sbin/sshd "$(command -v sshd 2>/dev/null)"; do [ -x "$c" ] && SSHD=$c && break; done
if [ -z "$SSHD" ]; then
    echo "  (sshd not installed: the client core was tested against the server core only)"
else
    ssh-keygen -q -t ecdsa -N '' -f sd_ecdsa; ssh-keygen -q -t rsa -b 3072 -N '' -f sd_rsa
    ./client -p 1 -i devkey x@127.0.0.1 true >/dev/null 2>&1     # makes devkey and devkey.pub
    cp devkey.pub sd_authorized_keys
    SPORT=""
    for try in 1 2 3 4 5 6; do
        p=$((20000 + RANDOM % 30000))
        cat > sshd_config <<EOF
Port $p
ListenAddress 127.0.0.1
HostKey $W/sd_ecdsa
HostKey $W/sd_rsa
AuthorizedKeysFile $W/sd_authorized_keys
UsePAM no
StrictModes no
PidFile $W/sshd.pid
PasswordAuthentication no
KbdInteractiveAuthentication no
EOF
        "$SSHD" -D -e -f sshd_config > sshd.log 2>&1 &
        for _ in $(seq 50); do grep -q "listening" sshd.log 2>/dev/null && break; sleep 0.05; done
        if grep -q "listening" sshd.log; then SPORT=$p; break; fi
        sleep 0.2
    done
    if [ -z "$SPORT" ]; then
        echo "  (sshd would not start unprivileged here: $(head -1 sshd.log))"
    else
        me=$(id -un)
        out=$(./client -p "$SPORT" -i devkey -k sd_kh "$me@127.0.0.1" 'echo from-sshd; exit 5' 2>sdc.err); rc=$?
        check "client core -> OpenSSH sshd: '$out' ($rc)" test "$out" = "from-sshd" -a $rc -eq 5
        check "sshd's ECDSA host key fingerprint" grep -q "$(ssh-keygen -lf sd_ecdsa.pub | awk '{print $2}')" sdc.err
        for alg in rsa-sha2-512 rsa-sha2-256; do
            out=$(./client -p "$SPORT" -i devkey -H $alg "$me@127.0.0.1" "echo $alg" 2>sdc.err); rc=$?
            check "client core verifies an RSA host key with $alg ($rc)" test "$out" = "$alg"
        done
        check "sshd's RSA host key fingerprint" grep -q "$(ssh-keygen -lf sd_rsa.pub | awk '{print $2}')" sdc.err
        out=$(printf 'echo tty-ok\nexit 6\n' | ./client -p "$SPORT" -i devkey -t "$me@127.0.0.1" 2>/dev/null); rc=$?
        check "client core: interactive session on sshd ($rc)" test $rc -eq 6 -a -n "$(grep 'tty-ok' <<<"$out")"
        got=$(./client -p "$SPORT" -i devkey "$me@127.0.0.1" "head -c 102400 /dev/zero" 2>/dev/null | wc -c)
        check "client core: 100 KB from sshd ($got bytes)" test "$got" -eq 102400
    fi
fi

# --- the device's own server code (os/shell/sshd.cpp) ---------------------------------
#
# sshd.cpp itself, built against host fakes for the socket and the storage
# (sshd_host.cpp) with the real scheduler, accounts, identity, guard and output
# layer: the network task, the session shells, the sink and its spill, Ctrl+C
# into the right session, and every way a connection ends — including the
# identity rule end to end, with root signed in at the "console" throughout.
kill "$(cat server.pid)" 2>/dev/null; rm -f server.pid
g++ "${CXXF[@]}" -DRPC_HAS_WIFI=1 -I"$OS/../loader-spike/firmware" "$HERE/sshd_host.cpp" "$OS/shell/sshd.cpp" \
    "$OS/shell/fsguard.cpp" "$CORE/sshserver.cpp" "$CORE/sshsess.cpp" "$CORE/lineedit.cpp" \
    "$CORE/interrupt.cpp" "$CORE/ident.cpp" "$CORE/users.cpp" "$CORE/sha256.cpp" "$CORE/perms.cpp" \
    "$CORE/protect.cpp" "$CORE/path.cpp" "$CORE/out.cpp" "$CORE/logring.cpp" "$CORE/registry.cpp" \
    "$CORE/sshwire.cpp" "$CORE/sshcrypto.cpp" "$CORE/sshtrans.cpp" "$CORE/sshkeys.cpp" \
    "$CORE/lock.cpp" "$CORE/task.cpp" "$CORE/blackbox.cpp" "$W/libmbed.a" -o "$W/sshd_host" || exit 1
mkdir -p droot
DPORT=""
for try in 1 2 3 4 5 6; do
    p=$((20000 + RANDOM % 30000))
    ./sshd_host "$p" droot authorized_keys > dserver.out 2> dserver.err &
    echo $! > server.pid
    for _ in $(seq 100); do grep -q READY dserver.out 2>/dev/null && break; sleep 0.05; done
    if grep -q READY dserver.out 2>/dev/null; then DPORT=$p; break; fi
    kill "$(cat server.pid)" 2>/dev/null; rm -f server.pid
done
[ -n "$DPORT" ] || { echo "    FAIL: sshd_host would not start"; tail -5 dserver.err; exit 1; }
D="-p $DPORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
   -o IdentitiesOnly=yes -o ConnectTimeout=10"
DK="$D -o BatchMode=yes -i id_ecdsa"
pw() {   # pw <password> <user> <command>: a password login through ssh's askpass
    printf '#!/bin/sh\necho %s\n' "$1" > askpw.sh; chmod +x askpw.sh
    SSH_ASKPASS="$W/askpw.sh" SSH_ASKPASS_REQUIRE=force setsid -w timeout 60 \
        ssh $D -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        -o NumberOfPasswordPrompts=1 "$2@127.0.0.1" "$3" </dev/null 2>&1
}

out=$(run $DK root@127.0.0.1 whoami 2>&1)
check "sshd.cpp: root by key: '$out'" test "$out" = "root"
out=$(run $DK root@127.0.0.1 readsecret 2>&1)
check "sshd.cpp: root may read the account file: '$out'" test "$out" = "allowed"
out=$(run $DK root@127.0.0.1 isadmin 2>&1)
check "sshd.cpp: root is an admin over SSH" test "$out" = "admin"

# The identity rule end to end: bob over SSH while root is at the console.
out=$(pw bobpw bob whoami)
check "sshd.cpp: bob by password: '$out'" test "$out" = "bob"
out=$(pw bobpw bob readsecret); rc=$?
check "sshd.cpp: bob cannot read the account file while root is at the console ($rc)" \
      test $rc -eq 1 -a -n "$(grep 'Permission denied' <<<"$out")" -a -z "$(grep allowed <<<"$out")"
out=$(pw bobpw bob 'write /etc/startup.cfg'); rc=$?
check "sshd.cpp: bob cannot change what runs at boot ($rc)" test $rc -eq 1 -a ! -e droot/etc/startup.cfg
out=$(pw bobpw bob 'write /home/root/.ssh/authorized_keys'); rc=$?
check "sshd.cpp: bob cannot add himself to root's keys ($rc)" test $rc -eq 1 -a -z "$(grep -x x droot/home/root/.ssh/authorized_keys)"
out=$(pw bobpw bob 'write /home/bob/notes.txt'); rc=$?
check "sshd.cpp: bob can write his own files ($rc: '$out')" test $rc -eq 0 -a "$out" = "written"
out=$(pw bobpw bob isadmin); rc=$?
check "sshd.cpp: bob is not an admin over SSH ($rc: '$out')" test $rc -eq 1 -a "$out" = "not admin"
out=$(pw bobpw bob bgjob)
for _ in $(seq 40); do [ -s droot/tmp/bgjob ] && break; sleep 0.1; done
check "sshd.cpp: a job bob starts is bob's, after he has gone: '$(cat droot/tmp/bgjob 2>/dev/null)'" \
      test "$(cat droot/tmp/bgjob 2>/dev/null)" = "bob remote"
out=$(pw wrong bob whoami); rc=$?
check "sshd.cpp: a wrong password is refused ($rc)" test $rc -eq 255
# guest has no password: refused with any password, AND with a key it lists.
out=$(pw anything guest whoami); rc=$?
check "sshd.cpp: a password-less account is refused by password ($rc)" test $rc -eq 255
out=$(run $DK guest@127.0.0.1 whoami 2>&1); rc=$?
check "sshd.cpp: ...and by a key in its own authorized_keys ($rc)" test $rc -eq 255
check "sshd.cpp: refusals are logged" grep -q "refused password for 'bob'" dserver.err
# A refused password holds its connection still for the pause, slot and all,
# even once the client has given up. Wait for those to end before going on.
settle() {
    for _ in $(seq 100); do
        [ "$(grep 'open sockets' dserver.err | tail -1)" = "open sockets: 1" ] && return 0
        sleep 0.1
    done
    return 1
}
check "sshd.cpp: held connections end and give their sockets back" settle

run $DK root@127.0.0.1 false >/dev/null 2>&1; rc=$?
check "sshd.cpp: exit status ($rc)" test $rc -eq 1
got=$(run $DK root@127.0.0.1 'big 102400' | sha256sum | cut -d' ' -f1)
check "sshd.cpp: 100 KB through the sink, the ring and the pump" test "$got" = "$(pattern 102400)"
t0=$(date +%s)
out=$(run $DK root@127.0.0.1 'locked 40000' 2>&1); rc=$?
t1=$(date +%s)
check "sshd.cpp: output under a lock is set aside, then cut with a note ($rc, $((t1 - t0))s)" \
      test $rc -eq 0 -a -n "$(grep -F '[ssh: output lost]' <<<"$out")" -a $((t1 - t0)) -lt 10
check "sshd.cpp: ...and what was kept is the start of it, unbroken" \
      test "$(head -c 4096 <<<"$out" | sha256sum | cut -d' ' -f1)" = "$(python3 -c "
import hashlib
p='0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/'
print(hashlib.sha256(''.join('\n' if j%64==63 else p[j%64] for j in range(4096)).encode()).hexdigest())")"

out=$(printf 'whoami\ncd /tmp\nexit\n' | run $D -tt -o BatchMode=yes -i id_ecdsa root@127.0.0.1 2>/dev/null); rc=$?
check "sshd.cpp: an interactive session ($rc)" test $rc -eq 0 -a -n "$(grep 'root@host:/home/root>' <<<"$out")"
check "sshd.cpp: ...prints the welcome line" grep -q "Signed in as root" <<<"$out"

# Ctrl+C reaches the command in its own session, and not one next to it.
run $DK root@127.0.0.1 'sleep 4' > beside.out 2>&1 &
pbeside=$!
python3 - "$DPORT" <<'PY' > dctrlc.out 2>&1
import subprocess, sys, time
p = subprocess.Popen(["ssh", "-tt", "-p", sys.argv[1], "-o", "StrictHostKeyChecking=no",
     "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "BatchMode=yes",
     "-o", "IdentitiesOnly=yes", "-i", "id_ecdsa", "root@127.0.0.1"],
     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
t0 = time.time()
p.stdin.write(b"sleep 30\n"); p.stdin.flush()
time.sleep(1.5)
p.stdin.write(b"\x03"); p.stdin.flush()
time.sleep(0.5)
p.stdin.write(b"echo after\nexit\n"); p.stdin.flush()
out, _ = p.communicate(timeout=30)
print(out.decode(errors="replace").replace("\r", ""))
print("ELAPSED %.1f" % (time.time() - t0))
PY
wait $pbeside
check "sshd.cpp: Ctrl+C interrupted the session's command" grep -q "Interrupted" dctrlc.out
check "sshd.cpp: the session carried on" grep -q "^after" dctrlc.out
check "sshd.cpp: the session beside it was not interrupted" test "$(cat beside.out)" = "slept"

# A client that vanishes mid-command: the session is torn down and its
# socket given back, three times over.
{ for i in 1 2 3; do timeout -s KILL 1.5 ssh $DK root@127.0.0.1 'sleep 30' >/dev/null 2>&1; done; } 2>/dev/null
sleep 1
run $DK root@127.0.0.1 'sleep 1' > a.out 2>&1 & pa=$!
run $DK root@127.0.0.1 'sleep 1' > b.out 2>&1 & pb=$!
wait $pa; ra=$?; wait $pb; rb=$?
check "sshd.cpp: both slots free again after clients vanished ($ra/$rb)" test $ra -eq 0 -a $rb -eq 0
# A third while two are busy is turned away.
settle
run $DK root@127.0.0.1 'sleep 5' >/dev/null 2>&1 & pa=$!
run $DK root@127.0.0.1 'sleep 5' >/dev/null 2>&1 & pb=$!
for _ in $(seq 50); do
    [ "$(grep 'open sockets' dserver.err | tail -1)" = "open sockets: 3" ] && break
    sleep 0.1
done
sleep 0.5
run $DK root@127.0.0.1 whoami >/dev/null 2>&1; rc=$?
wait $pa; wait $pb
check "sshd.cpp: a third connection is turned away ($rc)" test $rc -eq 255 -a -n "$(grep 'turned away' dserver.err)"
sleep 1
check "sshd.cpp: every socket was given back ($(grep 'open sockets' dserver.err | tail -1))" \
      test "$(grep 'open sockets' dserver.err | tail -1)" = "open sockets: 1"

# Stopping the server from inside one of its own sessions.
run $DK root@127.0.0.1 'ssh server off' >/dev/null 2>&1; rc=$?
sleep 1
run $DK root@127.0.0.1 whoami >/dev/null 2>&1; rc2=$?
check "sshd.cpp: 'ssh server off' from inside a session ($rc), then nothing listens ($rc2)" \
      test $rc2 -eq 255
check "sshd.cpp: ...and the listener was closed too" \
      test "$(grep 'open sockets' dserver.err | tail -1)" = "open sockets: 0"

echo "  $pass checks passed, $fail failed"
[ "$fail" -eq 0 ]
