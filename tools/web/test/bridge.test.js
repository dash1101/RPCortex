// The browser tool's half of the network bridge (LINK.md protocol 3), against
// the JavaScript mock device and a real HTTP server on 127.0.0.1.
//
//   deno run --allow-net=127.0.0.1 tools/web/test/bridge.test.js
//   node tools/web/test/bridge.test.js        (Node 20 or later)
//
// The mock has no network of its own (netless) and asks for every URL in the
// middle of a command, at its prompt (the terminal) or in an EXEC (link mode);
// the page's Bridge answers with fetch(). Nothing leaves this computer.

import { enterLink, Bridge, TermBridge, Frame, LinkError } from "../js/link.js";
import { MockDevice, makeApp } from "../js/mockdev.js";

const FAST = { settle: 40, enter: 1500, request: 2000, transfer: 2000, abort: 3000, bye: 400 };
const enc = new TextEncoder();
const dec = new TextDecoder();
const tests = [];
const test = (name, fn) => tests.push({ name, fn });
function eq(a, b, what) {
  const x = JSON.stringify(a), y = JSON.stringify(b);
  if (x !== y) throw new Error(`${what || "values differ"}: ${x} != ${y}`);
}
function ok(v, what) { if (!v) throw new Error(what || "expected true"); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function sha256hex(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");
}

// --- the site ------------------------------------------------------------------

const BIG = new Uint8Array(100 * 1024 + 3).map((_, i) => (i * 37 + 11) & 0xff);
const APP = makeApp("greet2", "2.1", 3000);
let BASE = "";

async function route(path) {
  if (path === "/small.txt") return [200, enc.encode("hello through the bridge\n"), {}];
  if (path === "/big.bin") return [200, BIG, {}];
  if (path === "/pkg/greet2.app") return [200, APP, {}];
  if (path === "/index.json") {
    return [200, enc.encode(JSON.stringify({ packages: [{ name: "greet2", ver: "2.1", url: BASE + "/pkg/greet2.app", sha256: await sha256hex(APP) }] })), { "content-type": "application/json" }];
  }
  if (path === "/moved") return [302, new Uint8Array(0), { location: BASE + "/small.txt" }];
  return [404, enc.encode("not here\n"), {}];
}

async function startServer() {
  if (globalThis.Deno) {
    const srv = globalThis.Deno.serve({ hostname: "127.0.0.1", port: 0, onListen() {} }, async (req) => {
      const [code, body, h] = await route(new URL(req.url).pathname);
      return new Response(code === 302 ? null : body, { status: code, headers: h });
    });
    BASE = `http://127.0.0.1:${srv.addr.port}`;
    return () => srv.shutdown();
  }
  const http = await import("node:http");
  const srv = http.createServer(async (req, res) => {
    const [code, body, h] = await route(req.url);
    res.writeHead(code, h);
    res.end(Buffer.from(body));
  });
  await new Promise((r) => srv.listen(0, "127.0.0.1", r));
  BASE = `http://127.0.0.1:${srv.address().port}`;
  return () => srv.close();
}

// --- helpers -------------------------------------------------------------------

async function linked(opts = {}, bridgeOpts = {}) {
  const dev = new MockDevice({ proto: 3, netless: true, ...opts }).start();
  dev.repoIndex = BASE + "/index.json";
  const log = [];
  // The server is on this computer, which the bridge refuses unless named.
  const bridge = new Bridge({ log: (l) => log.push(l), allow: ["127.0.0.1"], ...bridgeOpts });
  const r = await enterLink(dev.stream(), { timing: FAST, bridge });
  return { dev, link: r.link, bridge, log };
}

// The terminal: the device's bytes through TermBridge.feed, keys through key.
async function terminal() {
  const dev = new MockDevice({ proto: 3, netless: true }).start();
  dev.repoIndex = BASE + "/index.json";
  const s = dev.stream();
  const notes = [];
  const tb = new TermBridge(new Bridge({ allow: ["127.0.0.1"] }), (b) => s.write(b), { onNote: (n) => notes.push(n) });
  let shown = new Uint8Array(0);
  let running = true;
  (async () => {
    while (running) {
      const d = await s.read(50);
      if (d.length) { const out = tb.feed(d); const n = new Uint8Array(shown.length + out.length); n.set(shown); n.set(out, shown.length); shown = n; }
    }
  })();
  await tb.offer();
  return {
    dev, tb, notes,
    shown: () => shown,
    type: async (text) => { for (const b of enc.encode(text)) { await tb.key(new Uint8Array([b])); await sleep(2); } },
    stop: () => { running = false; dev.stop(); },
  };
}

async function until(fn, ms = 8000) {
  const end = Date.now() + ms;
  while (!fn() && Date.now() < end) await sleep(20);
  return fn();
}

// --- link mode -----------------------------------------------------------------

test("offered on entering link mode, to a board without a network only", async () => {
  const { dev, link } = await linked();
  eq(link.hello.bridge, "1");
  ok(link.bridged && dev.bridgeOffered, "offered");
  ok(dev.requests.includes("BRIDGE on"), "BRIDGE on sent");
  await link.bye();
  ok(dev.bridgeOffered, "the offer outlives BYE, for the terminal");
  dev.stop();
  const other = await linked({ netless: false });
  eq(other.link.hello.bridge, "0");
  ok(!other.link.bridged && !other.dev.requests.includes("BRIDGE on"), "not offered to a board with Wi-Fi");
  other.dev.stop();
});

test("wget and curl mid-EXEC", async () => {
  const { dev, link, bridge, log } = await linked();
  let r = await link.exec(`wget ${BASE}/big.bin /home/root/big.bin`);
  eq(r.rc, 0, r.text);
  eq(Array.from(dev.files.get("/home/root/big.bin")), Array.from(BIG), "big.bin intact");
  eq(dev.bridgeFrames.filter((f) => f.startsWith("FACK")).length, Math.ceil(BIG.length / 768), "one FACK per chunk");
  ok(log.some((l) => l.startsWith(`bridge: GET ${BASE}/big.bin`)), "logged");
  r = await link.exec(`curl ${BASE}/moved`);
  eq([r.rc, r.text], [0, "hello through the bridge\n"], "redirect followed by the browser");
  r = await link.exec(`curl ${BASE}/nothing`);
  eq(r.rc, 1);
  ok(r.text.includes("HTTP 404"), r.text);
  eq((await link.exec("echo after")).text, "after\n");
  eq(bridge.served.length, 3);
  await link.bye(); dev.stop();
});

test("sudo pkg install by name, through the bridge", async () => {
  const { dev, link } = await linked();
  link.sudoPassword = async () => "rootpw";
  const r = await link.exec(link.sudoLine("pkg install greet2"));
  eq(r.rc, 0, r.text);
  eq(dev.installed.greet2, "2.1");
  eq(dev.bridgeFetches.map((f) => f[1]), [BASE + "/index.json", BASE + "/pkg/greet2.app"]);
  await link.bye(); dev.stop();
});

test("allowlist, size cap, and the device stopping", async () => {
  let { dev, link } = await linked({}, { allow: ["example.com"] });
  let r = await link.exec(`curl ${BASE}/small.txt`);
  eq(r.rc, 1);
  ok(r.text.includes("allowlist"), r.text);
  await link.bye(); dev.stop();

  ({ dev, link } = await linked({}, { maxBytes: 4096 }));
  r = await link.exec(`wget ${BASE}/big.bin /home/root/b`);
  eq(r.rc, 1);
  ok(r.text.includes("bridge limit"), r.text);
  eq((await link.exec("echo after")).text, "after\n", "in step after the cap");
  await link.bye(); dev.stop();

  ({ dev, link } = await linked());
  dev.bridgeStopAfter = 3;
  r = await link.exec(`wget ${BASE}/big.bin /home/root/b`);
  eq(r.rc, 1);
  ok(dev.bridgeFrames.some((f) => f.startsWith("FSTOP")), "FSTOP");
  eq((await link.exec("echo after")).text, "after\n", "in step after FSTOP");
  await link.bye(); dev.stop();
});

// --- the Bridge on its own, with a device made of frames -------------------------

function fakeDevice(frames) {
  const q = frames.map((t) => new Frame(enc.encode(t)));
  const wrote = [];
  return {
    wrote,
    next: async () => (q.length ? q.shift() : null),
    push: (t) => q.push(new Frame(enc.encode(t))),
    write: async (b) => { wrote.push(dec.decode(b).replace(/^\x1e/, "").replace(/\n$/, "")); },
  };
}

test("only GET and HEAD, only http and https, CORS said plainly", async () => {
  const b = new Bridge();
  let d = fakeDevice(["FGO 1 chunk=768"]);
  await b.serve(new Frame(enc.encode("FETCH 1 POST https://example.com/x")), d.next, d.write);
  ok(d.wrote.some((l) => l.startsWith("FDONE 1 err refused") && l.includes("only GET and HEAD")), d.wrote.join("|"));
  d = fakeDevice(["FGO 2 chunk=768"]);
  await b.serve(new Frame(enc.encode("FETCH 2 GET file:///etc/passwd")), d.next, d.write);
  ok(d.wrote.some((l) => l.startsWith("FDONE 2 err refused") && l.includes("only http and https")), d.wrote.join("|"));
  const cors = new Bridge({ fetchFn: async () => { throw new TypeError("Failed to fetch"); } });
  d = fakeDevice(["FGO 3 chunk=768"]);
  await cors.serve(new Frame(enc.encode("FETCH 3 GET https://example.com/")), d.next, d.write);
  ok(d.wrote.some((l) => l.startsWith("FDONE 3 err net") && l.includes("CORS")), d.wrote.join("|"));
});

test("this computer and link-local addresses only when named", async () => {
  const b = new Bridge();
  for (const url of ["http://127.0.0.1:8/", "http://localhost/", "http://[::1]/", "http://169.254.169.254/"]) {
    let threw = false;
    try { b.check(url); } catch (e) { threw = true; }
    ok(threw, url + " refused");
  }
  new Bridge({ allow: ["127.0.0.1"] }).check("http://127.0.0.1:8/");
  const { dev, link } = await linked({}, { allow: [] });
  const r = await link.exec(`curl ${BASE}/small.txt`);
  eq(r.rc, 1);
  ok(r.text.includes("this computer itself") && r.text.includes("will not fetch that"), r.text);
  await link.bye(); dev.stop();
});

test("one chunk in flight: nothing more before the FACK", async () => {
  const body = new Uint8Array(3000).map((_, i) => i & 0xff);
  const b = new Bridge({ fetchFn: async () => new Response(body, { status: 200 }) });
  const q = [new Frame(enc.encode("FGO 4 chunk=768"))];
  const wrote = [];
  let unacked = 0, worst = 0;
  const next = async (ms) => {
    if (q.length) return q.shift();
    // Acknowledge only after looking at what was written since the last one.
    await sleep(5);
    if (unacked) { unacked = 0; return new Frame(enc.encode(`FACK 4 ${wrote.filter((l) => l.startsWith("FDATA")).length * 768}`)); }
    return null;
  };
  const write = async (bytes) => {
    const l = dec.decode(bytes).slice(1, -1);
    wrote.push(l);
    if (l.startsWith("FDATA")) { unacked++; worst = Math.max(worst, unacked); }
  };
  await b.serve(new Frame(enc.encode("FETCH 4 GET https://example.com/x")), next, write);
  eq(worst, 1, "never two chunks unacknowledged");
  eq(wrote.filter((l) => l.startsWith("FDATA")).length, 4);
  ok(wrote[wrote.length - 1] === "FDONE 4 ok", wrote.join("|"));
  ok(!wrote.some((l) => l.startsWith("FHEAD 4 200 len=")), "no len: the browser may have undone gzip");
});

// --- the terminal ----------------------------------------------------------------

test("terminal: the offer, wget at the prompt, frames off the screen", async () => {
  const t = await terminal();
  ok(await until(() => t.tb.offered), "the device answered the offer");
  ok(t.notes.some((n) => n.includes("network bridge on")), "said so");
  await t.type(`wget ${BASE}/small.txt /home/root/s.txt\r`);
  ok(await until(() => t.dev.files.has("/home/root/s.txt")), "saved");
  eq(dec.decode(t.dev.files.get("/home/root/s.txt")), "hello through the bridge\n");
  await sleep(100);
  const shown = dec.decode(t.shown());
  ok(shown.includes("Saved /home/root/s.txt"), shown);
  ok(!/\x1e(FETCH|FGO|FACK|BRIDGE)/.test(shown), "no bridge frame reached the screen");
  t.stop();
});

test("terminal: keys typed during a fetch wait for it, Ctrl+C does not", async () => {
  const writes = [];
  const tb = new TermBridge(new Bridge(), async (b) => { writes.push(Array.from(b)); });
  tb.fetching = true;
  await tb.key(enc.encode("ls"));
  await tb.key(new Uint8Array([3]));
  eq(writes, [[3]], "only Ctrl+C went out");
  await tb._release();
  eq(writes[1], Array.from(enc.encode("ls")), "the rest followed");
});

test("terminal: a stray answer never reaches the prompt", async () => {
  const t = await terminal();
  await until(() => t.tb.offered);
  await t.tb.key(enc.encode("\x1eFDATA 9 QUJD\nls /home\r"));
  ok(await until(() => t.dev.typedLines.includes("ls /home")), "the next line still runs");
  ok(!t.dev.typedLines.some((l) => l.includes("FDATA")), "the stray line did not");
  t.stop();
});

const stop = await startServer();
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
await stop();
console.log(`\n  ${tests.length - failed} passed, ${failed} failed`);
const exit = globalThis.process ? (c) => globalThis.process.exit(c) : (c) => globalThis.Deno.exit(c);
exit(failed ? 1 : 0);
