// Versions, compared the way the device compares them, so the page and
// `update check` / `pkg upgrade` never disagree about what is newer.
//
// No DOM here; the tests run it in Node and Deno.

const enc = new TextEncoder();
const DOT = 0x2e;
const INT_MAX = 0x7fffffff;

// C strings end at the first NUL, so a JavaScript string is cut there too.
function bytes(s) {
  const b = enc.encode(s == null ? "" : String(s));
  const z = b.indexOf(0);
  return z < 0 ? b : b.subarray(0, z);
}

// The number at the front of a component, saturating at INT32_MAX as
// version_digits() does rather than growing past what the device can hold.
function digits(s, i) {
  let v = 0;
  while (i < s.length && s[i] >= 0x30 && s[i] <= 0x39) {
    const d = s[i] - 0x30;
    v = v > Math.floor((INT_MAX - d) / 10) ? INT_MAX : v * 10 + d;
    i++;
  }
  return [v, i];
}

// repo_version_cmp() in os/core/repoindex.cpp, step for step, over the UTF-8
// bytes the device would see. Returns 1 when a is newer, 0 when equal, -1 when
// older. Missing components count as zero ("2.1" equals "2.1.0"); text after
// the digits of a component is a suffix, a component with one sorts newer than
// one without, and two suffixes compare byte by byte (as strncmp does).
export function versionCmp(a, b) {
  const A = bytes(a), B = bytes(b);
  let i = 0, j = 0;
  while (i < A.length || j < B.length) {
    let va, vb;
    [va, i] = digits(A, i);
    [vb, j] = digits(B, j);
    if (va !== vb) return va < vb ? -1 : 1;

    const si = i, sj = j;
    while (i < A.length && A[i] !== DOT) i++;
    while (j < B.length && B[j] !== DOT) j++;
    const la = i - si, lb = j - sj;
    if (la || lb) {
      if (!la) return -1;
      if (!lb) return 1;
      const n = Math.min(la, lb);
      for (let k = 0; k < n; k++) {
        if (A[si + k] !== B[sj + k]) return A[si + k] < B[sj + k] ? -1 : 1;
      }
      if (la !== lb) return la < lb ? -1 : 1;
    }
    if (A[i] === DOT) i++;
    if (B[j] === DOT) j++;
  }
  return 0;
}

// What the device compares a release against: RPC_OS_BUILDVER, which is the
// version without its "v" and label, then the build. Read from HELLO:
//
//   os=v2.0.640-beta build=640   ->  2.0.640    (vMAJOR.MINOR.BUILD, from 2026-11-21)
//   os=v2.05-beta    build=635   ->  2.05.635   (vMAJOR.ITER, v2.00 to v2.05)
//   os=v2.0.0        build=483   ->  2.0.0.483  (the frozen beta before that)
//
// `newScheme` is true when the build is already the last number of the
// version, which is the vMAJOR.MINOR.BUILD form.
export function deviceVersion(hello) {
  const os = String((hello && hello.os) || "").trim();
  const build = String((hello && hello.build) || "").trim();
  if (!os) return null;
  let num = os.replace(/^v/i, "");
  const dash = num.indexOf("-");
  if (dash >= 0) num = num.slice(0, dash);
  const newScheme = !!build && (num === build || num.endsWith("." + build));
  const cmp = build && !newScheme ? `${num}.${build}` : num;
  return { display: os, cmp, build: parseInt(build, 10) || 0, newScheme };
}

// The build in a manifest version: its last number ("2.0.640" -> 640).
export function buildOf(ver) {
  const parts = String(ver || "").split(".");
  return parseInt(parts[parts.length - 1], 10) || 0;
}

// Where this device stands against a release's manifest entry:
//
//   update   the release is newer, and `update check` says so
//   force    the release is newer by build, but the device numbered its
//            versions the old way and compares it as older, so it is only
//            installed with --force (this happens once, from v2.01 to v2.05)
//   current  the same build
//   ahead    the device runs something newer than the release (a build of its own)
//   unknown  the device did not say what it runs
export function verdict(hello, entry) {
  const mine = deviceVersion(hello);
  if (!mine || !entry || !entry.ver) return { state: "unknown", mine };
  const cmp = versionCmp(entry.ver, mine.cmp);
  let state;
  if (cmp > 0) state = "update";
  else if (!mine.newScheme && buildOf(entry.ver) > mine.build) state = "force";
  else if (cmp === 0) state = "current";
  else state = "ahead";
  return { state, mine, cmp, latestBuild: buildOf(entry.ver) };
}
