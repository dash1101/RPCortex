// The browser tool's protocol client against the JavaScript mock device.
//
//   node tools/web/test/link.test.js        (Node 20 or later)
//   deno run tools/web/test/link.test.js
//
// No dependencies and no browser: link.js and repo.js use only what Node,
// Deno and a browser all have (TextEncoder, crypto.subtle, atob/btoa).

import {
  enterLink, classify, quote, LinkError, DigestError, LoginRequired, installPackage, sha256hex,
} from "../js/link.js";
import { archRunsHere, boardArch, choose, parseIndex, download } from "../js/repo.js";
import { MockDevice, makeApp } from "../js/mockdev.js";

const FAST = { settle: 40, enter: 1500, request: 2000, transfer: 2000, abort: 2000, bye: 400 };
const enc = new TextEncoder();
const dec = new TextDecoder();
const tests = [];
const test = (name, fn) => tests.push({ name, fn });

function eq(a, b, what) {
  const x = JSON.stringify(a), y = JSON.stringify(b);
  if (x !== y) throw new Error(`${what || "values differ"}: ${x} != ${y}`);
}
function ok(v, what) { if (!v) throw new Error(what || "expected true"); }
async function rejects(p, cls, what) {
  try { await p; } catch (e) { if (!cls || e instanceof cls) return e; throw new Error(`${what}: wrong error ${e.name}: ${e.message}`); }
  throw new Error(`${what || "expected a rejection"}`);
}

async function connected(opts = {}, connectOpts = {}) {
  const dev = new MockDevice(opts).start();
  const s = dev.stream();
  const noise = [];
  const r = await enterLink(s, { timing: FAST, onNoise: (l) => noise.push(l), ...connectOpts });
  return { dev, s, r, link: r.link, noise };
}

test("enters link mode and reads HELLO", async () => {
  const { dev, link, r } = await connected();
  eq(r.mode, "link");
  eq(link.hello.board, "pico2_w");
  eq(link.chunk, 768);
  eq(dev.requests, []);
  ok(await link.bye());
  ok(!dev.inLink, "left in link mode");
  dev.stop();
});

test("exec returns output and status, chaining included", async () => {
  const { dev, link } = await connected({ noise: 0.4 });
  let x = await link.exec("echo hello");
  eq([x.rc, x.text], [0, "hello\n"]);
  x = await link.exec("fail && echo no || echo yes");
  eq([x.rc, x.text], [0, "[!] it failed\nyes\n"]);
  x = await link.exec("fail");
  eq(x.rc, 1);
  x = await link.exec("sleep 20", { timeout: 200 });
  ok(x.stopped, "not stopped");
  eq((await link.exec("echo after")).text, "after\n");
  await link.bye(); dev.stop();
});

test("put and get round trip, every size around the chunk, unicode path", async () => {
  const { dev, link } = await connected({ noise: 0.3 });
  for (const n of [0, 1, 767, 768, 769, 4000]) {
    const data = new Uint8Array(n).map((_, i) => (i * 31) & 0xff);
    const path = `/tmp/naïve ☃ ${n}`;
    let last = null;
    await link.put(data, path, { progress: (a, b) => { last = [a, b]; } });
    eq(last, [n, n], "progress");
    const back = await link.get(path);
    eq(Array.from(back), Array.from(data), `size ${n}`);
  }
  const names = (await link.ls("/tmp")).map((e) => e.name);
  ok(names.includes("naïve ☃ 768"), "listed");
  eq(await link.stat("/tmp/naïve ☃ 1"), { type: "f", size: 1 });
  eq(await link.stat("/nope"), null);
  await link.bye(); dev.stop();
});

test("errors come back as LinkError and leave the link usable", async () => {
  const { dev, link } = await connected();
  let e = await rejects(link.get("/nope"), LinkError, "get");
  eq(e.code, "nofile");
  e = await rejects(link.put(new Uint8Array(3), "/home"), LinkError, "put dir");
  eq(e.code, "nofile");
  dev.corruptGet = true;
  await rejects(link.get("/home/root/notes.txt"), DigestError, "damaged get");
  dev.corruptGet = false;
  await link.ping();
  await rejects(link.exec("echo " + "x".repeat(1200)), Error, "too long");
  await link.ping();
  await link.bye(); dev.stop();
});

test("cancelling an upload removes the partial file", async () => {
  const { dev, link } = await connected();
  const ctl = new AbortController();
  const data = new Uint8Array(8000);
  await rejects(link.put(data, "/tmp/partial", {
    signal: ctl.signal, progress: (a) => { if (a >= 1536) ctl.abort(); },
  }), Error, "cancel");
  ok(!dev.files.has("/tmp/partial"), "partial file kept");
  eq((await link.exec("echo ok")).text, "ok\n");
  await link.bye(); dev.stop();
});

test("requests made together run one at a time", async () => {
  const { dev, link } = await connected();
  const [a, b, c] = await Promise.all([link.exec("echo one"), link.ls("/home"), link.exec("echo three")]);
  eq([a.text, b.length, c.text], ["one\n", 2, "three\n"]);
  await link.bye(); dev.stop();
});

test("a device left in link mode is picked up again", async () => {
  const dev = new MockDevice().start();
  const s = dev.stream();
  await enterLink(s, { timing: FAST });             // and never says BYE
  const r = await enterLink(s, { timing: FAST });
  eq(r.mode, "link");
  eq((await r.link.exec("echo again")).text, "again\n");
  await r.link.bye(); dev.stop();
});

test("old firmware is reported, not driven", async () => {
  const dev = new MockDevice({ link: false }).start();
  const r = await enterLink(dev.stream(), { timing: FAST });
  eq(r.mode, "legacy");
  dev.stop();
});

test("login prompt: nothing typed without an account, then a real login", async () => {
  const dev = new MockDevice({ login: true }).start();
  await rejects(enterLink(dev.stream(), { timing: FAST }), LoginRequired, "login");
  eq(dev.loginTyped, []);
  const r = await enterLink(dev.stream(), { timing: FAST, user: "root", password: async () => "rootpw" });
  eq([r.mode, r.link.hello.user], ["link", "root"]);
  await r.link.bye(); dev.stop();
});

test("installing a package file asks for sudo once, and tries again when wrong", async () => {
  const { dev, link } = await connected();
  eq(link.proto, 2);
  const asked = [];
  link.sudoPassword = async (user, wrong) => { asked.push([user, wrong]); return asked.length === 1 ? "nope" : "rootpw"; };
  const x = await installPackage(link, makeApp("hello", "2.0"), "hello world.app");
  eq(x.rc, 0);
  eq(dev.installed.hello, "2.0");
  eq(asked, [["root", false], ["root", true]]);
  eq(dev.auths, ["nope", "rootpw"]);
  ok(dev.requests.some((r) => r.startsWith("EXEC sudo pkg install")), "sent with sudo");
  ok(![...dev.files.keys()].some((p) => p.startsWith("/tmp/")), "temp file left behind");
  // The password is kept: the next one asks nothing.
  await installPackage(link, makeApp("again", "1.0"), "again.app");
  eq(asked.length, 2);
  await link.bye(); dev.stop();
});

test("sudo: refused without a password, PUT into /etc after AUTH, nothing asked of older firmware", async () => {
  let { dev, link } = await connected();
  const e = await rejects(link.put(enc.encode("x"), "/etc/x"), Error, "no password");
  ok(/sudo/.test(e.message), e.message);
  await rejects(link.auth("wrong"), LinkError, "wrong");
  eq(await link.auth("rootpw"), 300);
  await link.put(enc.encode("x"), "/etc/x");
  eq(dec.decode(dev.files.get("/etc/x")), "x");
  const plain = await link.exec("pkg install /os/pkg/greet.app");
  ok(plain.rc === 1 && /Run it with sudo/.test(plain.text), "a line without sudo is the device's to refuse");
  await link.bye(); dev.stop();

  ({ dev, link } = await connected({ proto: 1 }));
  link.sudoPassword = async () => { throw new Error("asked"); };
  const x = await installPackage(link, makeApp("old", "1.0"), "old.app");
  eq(x.rc, 0);
  eq(dev.auths, []);
  ok(!dev.requests.some((r) => r.startsWith("AUTH") || r.includes("sudo")), "nothing new sent");
  await link.bye(); dev.stop();
});

test("classify and quote", async () => {
  eq(classify(enc.encode("^C\r\nroot@vela:/> ")), "shell");
  eq(classify(enc.encode("Username ••>  ")), "login");
  eq(classify(enc.encode("\x1eERR cancelled x\r\n")), "link");
  eq(classify(new Uint8Array(0)), null);
  eq(quote("/a/b"), "/a/b");
  eq(quote("a b"), '"a b"');
  eq(quote('say "hi"'), "'say \"hi\"'");
  let threw = false;
  try { quote("it's \"x\""); } catch (e) { threw = true; }
  ok(threw, "both quotes accepted");
});

test("repository rules match the device", async () => {
  const cases = [["", "armv6m", true], ["armv6m", "armv8m", true], ["armv8m", "armv6m", false],
    ["armv8m, xtensa", "armv6m", false], ["xtensa,armv6m", "armv6m", true]];
  for (const [a, m, want] of cases) eq(archRunsHere(a, m), want, `${a} on ${m}`);
  eq([boardArch("pico2_w"), boardArch("pico"), boardArch("x")], ["armv8m", "armv6m", null]);
  const blob = makeApp("t");
  const good = await sha256hex(blob);
  const idx = parseIndex({ packages: [
    { name: "T", url: "t6.app", arch: "armv6m", sha256: good },
    { name: "t", url: "t8.app", arch: "armv8m", sha256: good },
    { name: "bad", url: "t6.app", sha256: "0".repeat(64) },
    { name: "nourl" },
  ] }, "https://example.invalid/repo/index.json");
  eq(idx.length, 3);
  eq(choose(idx, "t", "armv8m").url, "https://example.invalid/repo/t8.app");
  eq(choose(idx, "t", "armv6m").url, "https://example.invalid/repo/t6.app");
  const fakeFetch = async () => ({ ok: true, arrayBuffer: async () => blob.buffer });
  eq((await download(idx[0], fakeFetch)).verified, true);
  await rejects(download(idx[2], fakeFetch), DigestError, "tampered");
});

let failed = 0;
for (const t of tests) {
  const t0 = Date.now();
  try {
    await t.fn();
    console.log(`  ok    ${t.name}  (${Date.now() - t0} ms)`);
  } catch (e) {
    failed++;
    console.log(`  FAIL  ${t.name}\n        ${e && e.stack ? e.stack.split("\n").slice(0, 3).join("\n        ") : e}`);
  }
}
console.log(`\n  ${tests.length - failed} passed, ${failed} failed`);
const exit = globalThis.process ? (c) => globalThis.process.exit(c) : (c) => globalThis.Deno.exit(c);
exit(failed ? 1 : 0);
