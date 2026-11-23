// The page's version comparison against the device's.
//
//   node tools/web/test/version.test.js
//   deno run tools/web/test/version.test.js
//
// The cases are os/host/repoindex_test.cpp's, line for line: if the C++ suite
// gains a case, it belongs here too, so the page and `update check` keep
// agreeing about which release is newer.

import { test, eq, ok, run } from "./harness.js";
import { versionCmp, deviceVersion, verdict, buildOf } from "../js/version.js";

const sign = (n) => (n > 0 ? ">" : n < 0 ? "<" : "==");

// [a, b, expected sign of repo_version_cmp(a, b), what the C++ test says]
const CPP = [
  ["1.0.0", "1.0.0", "==", "version: equal"],
  ["1.2.0", "1.10.0", "<", "version: 1.10 is newer than 1.2"],
  ["2.0.0", "1.99.9", ">", "version: major wins"],
  ["2.1", "2.1.0", "==", "version: missing component is zero"],
  ["3.0.0", "2.1.1", ">", "version: RPCMark 3.0.0 beats 2.1.1"],
  ["0.9.1", "1.0.0", "<", "version: 0.9.1 is older than 1.0.0"],
  ["1.0-beta", "1.0-beta", "==", "version: 1.0-beta equals 1.0-beta"],
  ["1.0b", "1.0", ">", "version: a suffix sorts newer, as before"],
  ["1.0", "1.0b", "<", "version: and the other way round"],
  ["1.0rc1", "1.0rc2", "<", "version: two suffixes compare as text"],
  ["1.0rc2", "1.0rc1", ">", "version: and the other way round"],
  ["1.0rc", "1.0rc1", "<", "version: a longer suffix with the same start is newer"],
  ["1.1rc1", "1.0", ">", "version: the number still decides before any suffix"],
  ["1.0", "1.1rc1", "<", "version: and the other way round"],
  ["2.0.0.463", "2.0.0.462", ">", "version: the build number is a fourth component"],
  ["2.0.486", "2.0.485", ">", "version: a later build of one iteration is newer"],
  ["2.0.486", "2.0.0.483", ">", "version: the new form is newer than any build of the old one"],
  ["2.0.0.483", "2.0.486", "<", "version: and an old manifest never beats a new device"],
  ["2.11.1203", "2.9.2000", ">", "version: the iteration compares as a number, not as text"],
  ["2.10.5", "2.9.99", ">", "version: v2.10 follows v2.9"],
  ["3.0.1", "2.99.9999", ">", "version: a new generation beats every iteration of the last"],
  ["2.0.486", "2.0.486", "==", "version: the same build is the same"],
  ["2.00.486", "2.0.486", "==", "version: a zero-padded minor is the same number"],
  ["2.00.486", "2.0.0.483", ">", "version: the padded form is newer than the old four-part one"],
  ["2.10.1", "2.09.999", ">", "version: v2.10 follows v2.09"],
  ["2.01.5", "2.00.999", ">", "version: the first patch beats every build of the release"],
  ["99999999999999999999", "1", ">", "version: a huge number saturates instead of wrapping"],
  ["1", "99999999999999999999", "<", "version: and the other way round"],
  ["4294967297", "2", ">", "version: a number past 32 bits does not wrap to a small one"],
  ["2", "4294967297", "<", "version: and the other way round"],
];

// "A version is never newer than itself", over the C++ suite's odd list.
const ODD = ["1.0-beta", "v1.0", "2.1rc1", "1.0b", "a.b", "-1", "0x10", "1.2.3-4", "", "1..2", "99999999999", "1.0.0.463"];

test("every case in repoindex_test.cpp", () => {
  const bad = [];
  for (const [a, b, want, what] of CPP) {
    const got = sign(versionCmp(a, b));
    if (got !== want) bad.push(`${what}: cmp("${a}", "${b}") is ${got}, the device says ${want}`);
  }
  for (const v of ODD) if (versionCmp(v, v) !== 0) bad.push(`"${v}" is not equal to itself`);
  ok(!bad.length, bad.join("\n        "));
  eq(CPP.length, 30, "the number of ported cases");
});

test("saturation and the corners C has and JavaScript does not", () => {
  // Both saturate at INT32_MAX, so they are equal to the device.
  eq(versionCmp("2147483647", "99999999999"), 0, "both saturate");
  eq(versionCmp("2147483646", "2147483647"), -1, "just below the limit");
  // A C string ends at its first NUL.
  eq(versionCmp("1.2\u0000junk", "1.2"), 0, "cut at NUL");
  // Suffixes compare as bytes, as strncmp does: UTF-8 bytes, unsigned.
  eq(versionCmp("1.0é", "1.0z"), 1, "0xC3 sorts after 'z'");
  eq(versionCmp("1.0-beta", "1.0-alpha"), 1, "text order of suffixes");
  eq(versionCmp(null, ""), 0, "nothing equals nothing");
  eq(versionCmp("1", null), 1, "something beats nothing");
});

test("the device's comparable version, from HELLO", () => {
  eq(deviceVersion({ os: "v2.0.640-beta", build: "640" }), { display: "v2.0.640-beta", cmp: "2.0.640", build: 640, newScheme: true });
  eq(deviceVersion({ os: "v2.05-beta", build: "635" }).cmp, "2.05.635");
  eq(deviceVersion({ os: "v2.01-beta", build: "554" }).newScheme, false);
  eq(deviceVersion({ os: "v2.00-beta", build: "483" }).cmp, "2.00.483");
  eq(deviceVersion({ os: "v2.0.0", build: "483" }).cmp, "2.0.0.483");
  eq(deviceVersion({ os: "v2.0.731", build: "731" }).cmp, "2.0.731");
  eq(deviceVersion({ os: "v2.1.800-rc2", build: "800" }).cmp, "2.1.800");
  eq(deviceVersion({}), null);
  eq([buildOf("2.0.640"), buildOf("2.05.635"), buildOf("")], [640, 635, 0]);
});

test("where a device stands against the release", () => {
  const latest = { ver: "2.0.640" };
  const v = (os, build) => verdict({ os, build }, latest).state;
  eq(v("v2.0.640-beta", "640"), "current", "the same build");
  eq(v("v2.0.600-beta", "600"), "update", "an earlier build of the new form");
  eq(v("v2.0.700-beta", "700"), "ahead", "a build of its own");
  // v2.01 to v2.05 compare 2.0x.<build> > 2.0.640: they need --force once.
  for (const [os, b] of [["v2.01-beta", "554"], ["v2.03-beta", "600"], ["v2.05-beta", "635"]]) {
    eq(v(os, b), "force", os);
    ok(versionCmp("2.0.640", deviceVersion({ os, build: b }).cmp) < 0, `${os} sees it as older`);
  }
  // v2.00-beta is 2.00.483, which is 2.0.483 to the device: it is offered the
  // update normally. Only a mirrored comparison gets this right; a rule that
  // named v2.01 to v2.05 would not have to.
  eq(v("v2.00-beta", "483"), "update", "v2.00-beta needs no force");
  eq(v("v2.0.0", "483"), "update", "the frozen beta needs no force");
  // The next minor is still "older" to v2.05 (1 < 5), so it needs the force
  // too; a device on v2.0.x is offered it normally.
  eq(verdict({ os: "v2.05-beta", build: "635" }, { ver: "2.1.800" }).state, "force", "v2.1 against v2.05");
  eq(verdict({ os: "v2.0.640-beta", build: "640" }, { ver: "2.1.800" }).state, "update", "v2.1 against v2.0.640");
  eq(verdict({ os: "v2.0.640-beta", build: "640" }, null).state, "unknown");
  eq(verdict({}, latest).state, "unknown");
});

run();
