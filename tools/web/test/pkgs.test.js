// The packages screen's logic against the mock device.
//
//   node tools/web/test/pkgs.test.js
//   deno run tools/web/test/pkgs.test.js

import { test, eq, ok, rejects, run } from "./harness.js";
import { enterLink, sha256hex, RpcError } from "../js/link.js";
import { parseIndex } from "../js/repo.js";
import { parseInstalled, readInstalled, plan, upgradeAll, removePackage, removeLine, linkArch } from "../js/pkgs.js";
import { MockDevice, makeApp } from "../js/mockdev.js";

const FAST = { settle: 40, enter: 1500, request: 2000, transfer: 2000, abort: 2000, bye: 400 };
const REPO = "https://example.invalid/repo-v2/";

async function connected(opts = {}) {
  const dev = new MockDevice(opts).start();
  const r = await enterLink(dev.stream(), { timing: FAST });
  return { dev, link: r.link };
}

// An index and the packages it points at, served by a pretend fetch.
async function repo(entries) {
  const blobs = {};
  const packages = [];
  for (const e of entries) {
    const blob = makeApp(e.name, e.ver, 900);
    const url = `packages/${e.name}-${e.arch || "any"}.app`;
    blobs[REPO + url] = e.tamper ? makeApp(e.name, e.ver, 901) : blob;
    packages.push({ name: e.name, ver: e.ver, desc: e.name + " does things", arch: e.arch || "", abi: e.abi || "1.27",
      size: blob.length, sha256: await sha256hex(blob), url });
  }
  const index = parseIndex({ name: "repo", format: 2, packages }, REPO + "index.json");
  const fetched = [];
  const fetchFn = async (url) => {
    fetched.push(url);
    return url in blobs ? new Response(blobs[url]) : new Response("no", { status: 404 });
  };
  return { index, fetchFn, fetched };
}

test("what is installed: _pkgs, and pkg list on firmware without it", async () => {
  const p = parseInstalled("PKGS_BEGIN\nOS:v2.0.640-beta:pico2_w\nPKG:greet:1.0\nPKG:novad1:1.8.0\nPKGS_END\n");
  eq(p, { os: "v2.0.640-beta", board: "pico2_w", list: [{ name: "greet", ver: "1.0" }, { name: "novad1", ver: "1.8.0" }] });
  const old = parseInstalled("[:] Installed packages:\n  \x1b[36mgreet           \x1b[0m 1.0     \n  calc             2.1\n");
  eq(old.list, [{ name: "greet", ver: "1.0" }, { name: "calc", ver: "2.1" }], "colour codes and padding dropped");
  const { dev, link } = await connected();
  dev.installed = { greet: "1.0", calc: "1.2" };
  eq((await readInstalled(link)).list.map((x) => x.name), ["calc", "greet"]);
  eq(dev.requests, ["EXEC _pkgs"], "one request, no sudo, nothing changed");
  await link.bye(); dev.stop();
});

test("the plan: versions compared as the device does, and the architecture rule", async () => {
  const { index } = await repo([
    { name: "greet", ver: "1.10", arch: "armv6m" },     // 1.10 is newer than 1.9: not a text compare
    { name: "calc", ver: "2.0", arch: "armv6m" },
    { name: "novad1", ver: "1.9.0", arch: "armv8m", abi: "1.27" },
    { name: "bench", ver: "1.0", arch: "armv6m" },
    { name: "Mine", ver: "0.9" },
    { name: "rp2350only", ver: "1.0", arch: "armv8m" },
  ]);
  const installed = [{ name: "greet", ver: "1.9" }, { name: "calc", ver: "2.0" }, { name: "novad1", ver: "1.8.0" },
    { name: "mine", ver: "1.0" }, { name: "stock", ver: "1.0" }];
  const big = plan(installed, index, "armv8m");
  const st = (p) => Object.fromEntries(p.rows.map((r) => [r.name, r.state]));
  eq(st(big), { calc: "current", greet: "upgrade", mine: "newer", novad1: "upgrade", stock: "unlisted" });
  eq(big.available.map((r) => [r.name, r.state]), [["bench", "available"], ["rp2350only", "available"]]);
  eq(big.rows.find((r) => r.name === "novad1").entry.abi, "1.27", "the ABI travels with the row");
  // On an RP2040, armv8m packages are not offered and not upgraded.
  const small = plan(installed, index, "armv6m");
  eq(st(small).novad1, "elsewhere");
  eq(small.available.find((r) => r.name === "rp2350only").state, "notforboard");
  eq([linkArch({ arch: "armv6m", board: "pico2_w" }), linkArch({ board: "pico_w" }), linkArch({ board: "esp32" })], ["armv6m", "armv6m", null]);
});

test("update all: sudo asked once up front, a wrong password asked again, every upgrade installed", async () => {
  const { dev, link } = await connected();
  dev.installed = { greet: "1.0", calc: "1.0", bench: "1.0" };
  const { index, fetchFn } = await repo([
    { name: "greet", ver: "1.1", arch: "armv6m" },
    { name: "calc", ver: "1.2", arch: "armv6m" },
    { name: "bench", ver: "1.0", arch: "armv6m" },
    { name: "novad1", ver: "1.9.0", arch: "armv8m" },
  ]);
  const asked = [];
  link.sudoPassword = async (u, wrong) => { asked.push(wrong); return asked.length === 1 ? "nope" : "rootpw"; };
  const p = plan((await readInstalled(link)).list, index, linkArch(link.hello));
  const steps = [];
  const r = await upgradeAll(link, p.rows, { fetchFn, onStep: (row, phase) => steps.push(row.name + " " + phase) });
  eq(r, { done: ["calc", "greet"], failed: [] });
  eq(dev.installed, { greet: "1.1", calc: "1.2", bench: "1.0" });
  eq(asked, [false, true], "asked, wrong, asked again; then never");
  eq(dev.auths, ["nope", "rootpw"]);
  eq(steps, ["calc start", "calc done", "greet start", "greet done"]);
  const auth = dev.requests.findIndex((q) => q.startsWith("AUTH"));
  const first = dev.requests.findIndex((q) => q.startsWith("PUT"));
  ok(auth >= 0 && auth < first, "the password before anything was sent");
  ok(dev.requests.includes("EXEC sudo pkg install /tmp/greet.app"), "installed with sudo");
  // The device's grace runs out: the kept password is sent again, nothing asked.
  dev.grace = false;
  dev.installed.greet = "1.0";
  await upgradeAll(link, plan((await readInstalled(link)).list, index, "armv8m").rows, { fetchFn });
  eq(dev.installed.greet, "1.1");
  eq(asked.length, 2, "no third question");
  await link.bye(); dev.stop();
});

test("update all: no password stops before anything is sent; a bad download is one failure, not all", async () => {
  let { dev, link } = await connected();
  dev.installed = { greet: "1.0", calc: "1.0" };
  let { index, fetchFn } = await repo([{ name: "greet", ver: "1.1" }, { name: "calc", ver: "1.2" }]);
  link.sudoPassword = async () => null;
  await rejects(upgradeAll(link, plan([{ name: "greet", ver: "1.0" }], index, "armv8m").rows, { fetchFn }), RpcError, "no password");
  ok(!dev.requests.some((q) => q.startsWith("PUT") || q.includes("pkg install")), "nothing sent");
  await link.bye(); dev.stop();

  ({ dev, link } = await connected());
  dev.installed = { greet: "1.0", calc: "1.0" };
  ({ index, fetchFn } = await repo([{ name: "greet", ver: "1.1", tamper: true }, { name: "calc", ver: "1.2" }]));
  link.sudoPassword = async () => "rootpw";
  const r = await upgradeAll(link, plan((await readInstalled(link)).list, index, "armv8m").rows, { fetchFn });
  eq(r, { done: ["calc"], failed: ["greet"] });
  eq(dev.installed, { greet: "1.0", calc: "1.2" }, "the tampered one was not installed");
  await link.bye(); dev.stop();
});

test("an RP2040 board is never sent an armv8m package", async () => {
  const { dev, link } = await connected({ board: "pico_w" });
  eq(link.hello.arch, "armv6m");
  dev.installed = { novad1: "1.0" };
  const { index, fetchFn, fetched } = await repo([{ name: "novad1", ver: "1.9.0", arch: "armv8m" }]);
  link.sudoPassword = async () => "rootpw";
  const p = plan((await readInstalled(link)).list, index, linkArch(link.hello));
  eq(p.rows[0].state, "elsewhere");
  eq(await upgradeAll(link, p.rows, { fetchFn }), { done: [], failed: [] });
  eq(fetched, [], "not even downloaded");
  await link.bye(); dev.stop();
});

test("remove: with sudo, and the device's refusal shown as it said it", async () => {
  const { dev, link } = await connected();
  dev.installed = { greet: "1.0", calc: "1.0" };
  link.sudoPassword = async () => "rootpw";
  eq(removeLine(link, "calc"), "sudo pkg remove calc");
  await removePackage(link, "calc");
  eq(Object.keys(dev.installed), ["greet"]);
  const e = await rejects(removePackage(link, "nothere"), RpcError, "not installed");
  ok(/Not installed: nothere/.test(e.message), e.message);
  await link.bye(); dev.stop();
});

run();
