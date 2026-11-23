// The firmware update flows against the mock device.
//
//   node tools/web/test/updates.test.js
//   deno run tools/web/test/updates.test.js

import { test, eq, ok, rejects, run } from "./harness.js";
import { enterLink, RpcError, DigestError, TimeoutError, sha256hex } from "../js/link.js";
import {
  parseManifest, manifestFor, loadReleases, uf2Expect, imageUrl, fetchBytes, binChip, uf2Chip,
  sniffImage, bootselChip, writeUf2, otaLine, otaUpdate, sendImage, runAtConsole, FROM_FILE, IMAGE_PATH, consoleText, fromFileExec,
  ROLLBACK_IMG,
} from "../js/updates.js";
import { verdict, deviceVersion } from "../js/version.js";
import { MockDevice, makeImage } from "../js/mockdev.js";

const FAST = { settle: 40, enter: 1500, request: 2000, transfer: 2000, abort: 2000, bye: 400 };
const enc = new TextEncoder();
const BASE = "https://example.invalid/releases/";

// latest.json as release.py writes it, for two boards.
const MANIFEST = {
  name: "RPCortex releases", format: 2, maintainer: "dash1101",
  packages: [
    { name: "pico2_w", ver: "2.0.640", desc: "RPCortex v2.0.640-beta Vela II for pico2_w", arch: "armv8m", size: 4096, sha256: "", url: BASE + "rpcortex-v2-pico2_w.bin" },
    { name: "pico2", ver: "2.0.640", desc: "RPCortex v2.0.640-beta Vela II for pico2", arch: "armv8m", size: 4096, sha256: "", url: BASE + "rpcortex-v2-pico2.bin" },
  ],
};

async function connected(opts = {}, timing = FAST) {
  const dev = new MockDevice({ manifest: MANIFEST, ...opts }).start();
  const s = dev.stream();
  const r = await enterLink(s, { timing });
  const asked = [];
  r.link.sudoPassword = async (user, wrong) => { asked.push([user, wrong]); return "rootpw"; };
  return { dev, s, link: r.link, asked };
}

// The device after it restarts: a new port, link mode again.
async function reconnect(dev) {
  dev.revive();
  const r = await enterLink(dev.stream(), { timing: FAST });
  return r.link;
}

function fakeFetch(files) {
  const calls = [];
  const f = async (url) => {
    calls.push(url);
    if (!(url in files)) return new Response("not here", { status: 404 });
    const v = files[url];
    return new Response(v instanceof Uint8Array ? v : JSON.stringify(v), { headers: { "content-length": String(v.length || 0) } });
  };
  f.calls = calls;
  return f;
}

// A vector table where each chip keeps it: at 0 on RP2350, at 0x100 on RP2040.
function fakeBin(chip, size = 2048) {
  const d = new Uint8Array(size);
  const v = new DataView(d.buffer);
  const at = chip === "RP2040" ? 0x100 : 0;
  v.setUint32(at, chip === "RP2040" ? 0x20042000 : 0x20082000, true);
  v.setUint32(at + 4, 0x10000201, true);
  d.set(enc.encode("RPCortex v2.0.640-beta rpcimage:standard"), 0x400);
  return d;
}

function fakeUf2(family, blocks = 3, absoluteFirst = false) {
  const d = new Uint8Array(512 * blocks);
  const v = new DataView(d.buffer);
  for (let i = 0; i < blocks; i++) {
    const o = i * 512;
    v.setUint32(o, 0x0a324655, true);
    v.setUint32(o + 4, 0x9e5d5157, true);
    v.setUint32(o + 8, 0x2000, true);
    v.setUint32(o + 28, absoluteFirst && i === 0 ? 0xe48bff57 : family, true);
    v.setUint32(o + 508, 0x0ab16f30, true);
  }
  return d;
}

// --- the release files --------------------------------------------------------

test("the release files: manifest, notes, boards, and the .uf2 checksum", async () => {
  const uf2 = fakeUf2(0xe48bff59);
  const sha = await sha256hex(uf2);
  const history = { schema: 1, releases: [
    { tag: "v2.0.640-beta", display: "v2.0.640-beta", version: "2.0", build: 640, date: "2026-11-21",
      notes: { fixed: ["a"] }, assets: [{ board: "pico2_w", kind: "uf2", size: uf2.length, sha256: sha }] },
    { tag: "v2.05-beta", display: "v2.05-beta", version: "2.05", build: 635, assets: [] },
  ] };
  const f = fakeFetch({ [BASE + "latest.json"]: MANIFEST, [BASE + "releases.json"]: history });
  const R = await loadReleases(BASE, f);
  eq(R.manifest.length, 2);
  eq(manifestFor(R.manifest, "PICO2_W").ver, "2.0.640", "found case-insensitively, as repo_find does");
  eq(R.history.length, 2);
  eq(R.boards, null, "boards.json missing");
  ok(/404/.test(R.errors.boards), "its error is kept: " + R.errors.boards);
  eq(uf2Expect(R.history, "pico2_w", 640).sha256, sha, "the uf2 checksum from the release with this build");
  eq(uf2Expect(R.history, "pico2_w", 635), null, "not from another release");
  eq(imageUrl(BASE, "pico2_w", "uf2"), BASE + "rpcortex-v2-pico2_w.uf2");
  await rejects(Promise.resolve().then(() => parseManifest({ nope: 1 })), RpcError, "not a manifest");
});

test("an image is refused unless it matches the release", async () => {
  const data = fakeBin("RP2350");
  const sha = await sha256hex(data);
  const f = fakeFetch({ [BASE + "x.bin"]: data });
  const seen = [];
  const got = await fetchBytes(BASE + "x.bin", { sha256: sha, fetchFn: f, progress: (a, t) => seen.push([a, t]) });
  ok(got.verified, "verified");
  eq(got.data.length, data.length);
  eq(seen[seen.length - 1], [data.length, data.length], "progress reaches the end");
  await rejects(fetchBytes(BASE + "x.bin", { sha256: "0".repeat(64), fetchFn: f }), DigestError, "tampered");
  await rejects(fetchBytes(BASE + "gone.bin", { fetchFn: f }), RpcError, "missing");
});

test("which chip an image is for, and which a BOOTSEL drive is", () => {
  eq([binChip(fakeBin("RP2350")), binChip(fakeBin("RP2040")), binChip(new Uint8Array(4096))], ["RP2350", "RP2040", null]);
  eq(sniffImage(fakeBin("RP2040")), { version: "v2.0.640-beta", variant: "standard", chip: "RP2040" });
  eq(uf2Chip(fakeUf2(0xe48bff56)), "RP2040");
  eq(uf2Chip(fakeUf2(0xe48bff59, 3, true)), "RP2350", "past the absolute block an RP2350 image starts with");
  eq(uf2Chip(new Uint8Array(512)), null, "not a uf2");
  eq(bootselChip("UF2 Bootloader v1.0\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n"), "RP2350");
  eq(bootselChip("UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n"), "RP2040");
  eq(bootselChip("hello"), null);
});

test("the console as a person sees it: CRLF ends a line, a lone CR redraws it", () => {
  const c = consoleText();
  c.feed(enc.encode("\x1b[36m[:]\x1b[0m Staging 848 KB...\r\n"));
  c.feed(enc.encode("Staging [###     ]\r"));
  c.feed(enc.encode("\nnext\r\nStaging [##      ]\rStaging [########]"));
  c.feed(enc.encode("\r"));                     // a CR at the end of one read...
  c.feed(enc.encode("\n\x1eOK rc=0\ndone"));     // ...and its LF at the start of the next
  eq(c.text(), "[:] Staging 848 KB...\nStaging [###     ]\nnext\nStaging [########]\ndone");
});

test("writing a .uf2 onto a BOOTSEL drive", async () => {
  const drive = (info, { closeFails = false } = {}) => {
    const files = {};
    return {
      name: "RP2350", files,
      async getFileHandle(n, o = {}) {
        if (n === "INFO_UF2.TXT") {
          if (info === null) throw new Error("NotFoundError");
          return { getFile: async () => ({ text: async () => info }) };
        }
        if (!o.create) throw new Error("NotFoundError");
        return {
          createWritable: async () => ({
            write: async (d) => { files[n] = d; },
            close: async () => { if (closeFails) throw new Error("the drive went away"); },
            abort: async () => {},
          }),
        };
      },
    };
  };
  const img = fakeUf2(0xe48bff59, 4, true);
  let d = drive("Board-ID: RP2350\n");
  eq(await writeUf2(d, img, { chip: "RP2350" }), { vanished: false });
  eq(d.files["rpcortex.uf2"].length, img.length);
  d = drive("Board-ID: RP2350\n", { closeFails: true });
  eq(await writeUf2(d, img), { vanished: true }, "the drive going as the board restarts is success");
  d = drive("Board-ID: RPI-RP2\n");
  await rejects(writeUf2(d, img), RpcError, "an RP2350 image onto an RP2040");
  eq(Object.keys(d.files), [], "nothing written");
  await rejects(writeUf2(drive(null), img), RpcError, "a folder that is not a BOOTSEL drive");
  await rejects(writeUf2(drive("Board-ID: RP2350\n"), img, { chip: "RP2040" }), RpcError, "the board was the other chip");
});

// --- over the air -------------------------------------------------------------

test("over the air: sudo once, update online --yes, the restart, the new version", async () => {
  const { dev, link, asked } = await connected({ os: "v2.0.600-beta", build: "600" });
  eq(verdict(link.hello, manifestFor(parseManifest(MANIFEST), "pico2_w")).state, "update");
  const r = await otaUpdate(link, { gone: () => dev.gone });
  eq(r, { restarted: true });
  eq(asked, [["root", false]], "asked once, before the download");
  ok(dev.requests.includes("EXEC sudo update online --yes"), "sent with sudo and --yes: " + dev.requests.join(" | "));
  eq(dev.updates, [{ sub: "online", force: false, yes: true }]);
  const back = await reconnect(dev);
  eq(back.hello.os, "v2.0.640-beta");
  eq(verdict(back.hello, MANIFEST.packages[0]).state, "current", "and now it is up to date");
  await back.bye(); dev.stop();
});

test("over the air from v2.05-beta: it will not offer the release, --force moves it across", async () => {
  const { dev, link } = await connected({ os: "v2.05-beta", build: "635" });
  const entry = manifestFor(parseManifest(MANIFEST), "pico2_w");
  eq(verdict(link.hello, entry).state, "force");
  eq(deviceVersion(link.hello).cmp, "2.05.635");
  // What the page would see if it did not know: the device calls it older.
  const plain = await otaUpdate(link, { gone: () => dev.gone });
  ok(!plain.restarted && plain.upToDate, "the device says it is already running it");
  ok(plain.canForce, "and says --force does it");
  const forced = await otaUpdate(link, { force: true, gone: () => dev.gone });
  eq(forced, { restarted: true });
  ok(dev.requests.includes("EXEC sudo update online --force --yes"), dev.requests.join(" | "));
  const back = await reconnect(dev);
  eq([back.hello.os, back.hello.build], ["v2.0.640-beta", "640"]);
  eq(verdict(back.hello, entry).state, "current", "after the move, the ordinary comparison works");
  await back.bye(); dev.stop();
});

test("over the air: no radio, no network and a failed download are shown, not a restart", async () => {
  let { dev, link } = await connected({ board: "pico2" });
  let r = await otaUpdate(link, { gone: () => dev.gone });
  ok(!r.restarted && r.refused && /no wireless hardware/.test(r.text), r.text);
  await link.bye(); dev.stop();

  ({ dev, link } = await connected({ network: false, os: "v2.0.600-beta", build: "600" }));
  r = await otaUpdate(link, { gone: () => dev.gone });
  ok(r.refused && /No network/.test(r.text), r.text);
  dev.network = true;
  dev.updateFails = "Checksum mismatch. Nothing was written.";
  r = await otaUpdate(link, { gone: () => dev.gone });
  ok(r.refused && /Checksum mismatch/.test(r.text), r.text);
  eq(dev.restarts, 0);
  await link.bye(); dev.stop();
});

test("over the air: a wrong password is asked again, and no password sends nothing", async () => {
  let { dev, link } = await connected({ os: "v2.0.600-beta", build: "600" });
  const asked = [];
  link.sudoPassword = async (u, wrong) => { asked.push(wrong); return asked.length === 1 ? "nope" : "rootpw"; };
  eq(await otaUpdate(link, { gone: () => dev.gone }), { restarted: true });
  eq(asked, [false, true]);
  eq(dev.auths, ["nope", "rootpw"]);
  dev.stop();

  ({ dev, link } = await connected({ os: "v2.0.600-beta", build: "600" }));
  link.sudoPassword = async () => null;
  await rejects(otaUpdate(link, { gone: () => dev.gone }), RpcError, "no password");
  ok(!dev.requests.some((q) => q.includes("update")), "nothing reached the device");
  await link.bye(); dev.stop();
});

test("why --yes: without it the device waits for an answer EXEC never sends", async () => {
  const { dev, link } = await connected({ os: "v2.0.600-beta", build: "600" }, { ...FAST, abort: 400, bye: 200 });
  eq(otaLine(link, false), "sudo update online --yes");
  await link.authenticate();
  // Ctrl+C after 300 ms; the question ignores it, as the console's does.
  await rejects(link.exec("sudo update online", { timeout: 300 }), TimeoutError, "it should wait");
  eq(dev.restarts, 0);
  dev.stop();
});

// --- from this page ---------------------------------------------------------------

test("from the page: the image goes to /os/update.bin with sudo, then from-file at the console", async () => {
  const { dev, s, link, asked } = await connected({ os: "v2.0.600-beta", build: "600" });
  const img = makeImage("pico2_w", "v2.0.640-beta", "640", 5000);
  dev.files.set(IMAGE_PATH, new Uint8Array(10));           // a stale one from before
  const seen = [];
  await sendImage(link, img, { progress: (a, t) => seen.push([a, t]) });
  eq(dev.files.get(IMAGE_PATH).length, img.length, "on the device");
  eq(seen[seen.length - 1], [img.length, img.length]);
  eq(asked.length, 1, "sudo asked once");
  ok(dev.requests.includes("EXEC sudo rm /os/update.bin"), "the stale one removed first");
  ok(dev.requests.some((q) => q.startsWith(`PUT ${img.length} `) && q.endsWith(" " + IMAGE_PATH)), "PUT to the staging path");
  // The console: link mode ends (and its grace with it), so sudo asks there.
  await link.bye();
  const echoed = [];
  const r = await runAtConsole(s, link.sudoLine(FROM_FILE), { password: "rootpw", echo: (b) => echoed.push(b), settle: 80 });
  ok(r.restarted, "it restarted: " + r.text);
  ok(/Password for root/.test(r.text) && /\(yes\/no\)/.test(r.text), "both questions were asked and answered");
  ok(echoed.length > 0, "the person saw it");
  eq(dev.updates, [{ sub: "from-file", path: IMAGE_PATH }]);
  ok(!dev.files.has(IMAGE_PATH), "the update removes its own staging file");
  const back = await reconnect(dev);
  eq(back.hello.os, "v2.0.640-beta");
  await back.bye(); dev.stop();
});

test("from the page, current firmware: from-file --yes through EXEC, no console needed", async () => {
  const { dev, link } = await connected({ os: "v2.0.650-beta", build: "650" });
  const img = makeImage("pico2_w", "v2.0.660-beta", "660", 5000);
  await sendImage(link, img, {});
  const r = await fromFileExec(link, { gone: () => dev.gone });
  ok(r.restarted, "it restarted: " + JSON.stringify(r));
  ok(dev.requests.includes("EXEC sudo update from-file /os/update.bin --yes"), dev.requests.join(" | "));
  eq(dev.updates, [{ sub: "from-file", path: IMAGE_PATH }]);
  const back = await reconnect(dev);
  eq(back.hello.os, "v2.0.660-beta");
  await back.bye(); dev.stop();
});

test("from the page, firmware from before --yes: EXEC says so, nothing is written", async () => {
  const { dev, link } = await connected({ os: "v2.0.640-beta", build: "640", fromFileYes: false });
  const img = makeImage("pico2_w", "v2.0.660-beta", "660", 5000);
  await sendImage(link, img, {});
  const r = await fromFileExec(link, { gone: () => dev.gone });
  eq(r.unsupported, true, "the caller falls back to the console");
  eq(r.restarted, false);
  eq(dev.updates, [], "nothing was written");
  await link.bye(); dev.stop();
});

test("from the page: a password the page already checked is not asked for again", async () => {
  const { dev, link, asked } = await connected();
  // The Firmware tab asks once, sends AUTH itself, and keeps the password for
  // the console step; sending the image must not ask a second time.
  await link.auth("rootpw");
  await sendImage(link, makeImage("pico2_w", "v2.0.640-beta", "640"), { authed: true });
  eq(asked, [], "not asked");
  ok(dev.files.has(IMAGE_PATH), "sent under the grace AUTH started");
  await link.bye(); dev.stop();
});

test("from the page: no room beside the saved firmware asks first, then makes room", async () => {
  // Two images do not fit at once: on a board the littlefs overhead makes
  // that true of a 1792 KB filesystem; here it is a smaller one.
  let { dev, link } = await connected({ capacity: 1600 * 1024 });
  dev.files.set(ROLLBACK_IMG, new Uint8Array(870 * 1024));
  const img = makeImage("pico2_w", "v2.0.640-beta", "640", 868 * 1024);
  const offered = [];
  await rejects(sendImage(link, img, { confirmForget: async (n) => { offered.push(n); return false; } }), RpcError, "declined");
  eq(offered, [870 * 1024]);
  ok(dev.files.has(ROLLBACK_IMG) && !dev.files.has(IMAGE_PATH), "nothing changed when declined");
  await sendImage(link, img, { confirmForget: async () => true });
  ok(!dev.files.has(ROLLBACK_IMG), "the saved copy went");
  eq(dev.files.get(IMAGE_PATH).length, img.length, "and the image fits");
  ok(dev.requests.includes("EXEC sudo update rollback --forget"), "removed by the device's own command");
  await link.bye(); dev.stop();

  // Too big however it is done: said plainly, nothing removed.
  ({ dev, link } = await connected({ capacity: 600 * 1024 }));
  await rejects(sendImage(link, img, { confirmForget: async () => true }), RpcError, "no room at all");
  await link.bye(); dev.stop();
});

test("from the page: an image the device refuses brings the prompt back, not a restart", async () => {
  const { dev, s, link } = await connected();
  await sendImage(link, makeImage("pico", "v2.0.640-beta", "640"));     // another board's
  await link.bye();
  const r = await runAtConsole(s, link.sudoLine(FROM_FILE), { password: "rootpw", settle: 80 });
  ok(!r.restarted, "no restart");
  ok(/does not look like firmware/.test(r.text), r.text);
  eq(dev.restarts, 0);
  // The console is still usable afterwards.
  const again = await enterLink(s, { timing: FAST });
  eq(again.link.hello.os, "v2.01-beta");
  await again.link.bye(); dev.stop();
});

test("from the page: a password the console refuses is typed once, never again", async () => {
  const { dev, s, link } = await connected();
  await sendImage(link, makeImage("pico2_w", "v2.0.640-beta", "640"));
  await link.bye();
  dev.auths = [];
  const e = await rejects(runAtConsole(s, link.sudoLine(FROM_FILE), { password: "wrong", settle: 80, timeout: 5000 }), RpcError, "wrong password");
  ok(/did not take the password/.test(e.message), e.message);
  eq(dev.auths, ["wrong", "", ""], "typed once, then empty answers until sudo gives up");
  eq(dev.restarts, 0);
  dev.stop();
});

test("why the console: under EXEC, from-file waits for an answer nobody sends", async () => {
  const { dev, link } = await connected({}, { ...FAST, abort: 400, bye: 200 });
  await sendImage(link, makeImage("pico2_w", "v2.0.640-beta", "640"));
  await rejects(link.exec(link.sudoLine(FROM_FILE), { timeout: 300 }), TimeoutError, "it should wait");
  eq(dev.restarts, 0);
  dev.stop();
});

run();
