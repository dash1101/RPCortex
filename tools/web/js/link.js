// The link protocol client (LINK.md, protocol 1), for the browser and for Node.
//
// No DOM here. It talks to a byte stream with two methods:
//
//   write(Uint8Array) -> Promise
//   read(ms)          -> Promise<Uint8Array>   (empty after ms with nothing)
//
// One request at a time, read to its final OK or ERR before the next is sent;
// calls made while one is running wait their turn. Lines that are not frames
// are background output from the device and go to onNoise, never to a parser.

export const RS = 0x1e;
export const LINE_MAX = 1100;
export const DEFAULT_CHUNK = 768;

export const Timing = {
  settle: 250,      // after a Ctrl+C, before reading what came back
  enter: 4000,      // from `link` to the HELLO frame
  request: 10000,   // an ordinary request to its final frame
  transfer: 15000,  // one ACK, D or final frame during PUT and GET
  abort: 15000,     // a cancelled request to its final frame
  bye: 2000,
};

const enc = new TextEncoder();
const dec = new TextDecoder("utf-8");

export class RpcError extends Error {
  constructor(message) { super(message); this.name = "RpcError"; }
}
export class TimeoutError extends RpcError {
  constructor(message) { super(message); this.name = "TimeoutError"; }
}
export class LinkError extends RpcError {
  constructor(code, message) {
    super(`${message || code} (${code})`);
    this.name = "LinkError";
    this.code = code;
    this.detail = message;
  }
}
export class DigestError extends RpcError {
  constructor(message) { super(message); this.name = "DigestError"; }
}
export class LoginRequired extends RpcError {
  constructor(message) { super(message); this.name = "LoginRequired"; }
}
export class CancelledError extends RpcError {
  constructor() { super("cancelled"); this.name = "CancelledError"; }
}

// --- bytes, text, digests ----------------------------------------------------

export function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

const ANSI = /\x1b\[[0-9;?]*[ -\/]*[@-~]|\x1b[()][A-Za-z0-9]|\x1b[=>78]/g;
export function stripAnsi(text) { return text.replace(ANSI, ""); }

export function b64encode(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return btoa(s);
}

export function b64decode(text) {
  let s;
  try { s = atob(text.trim()); } catch (e) { throw new RpcError("bad base64 from the device"); }
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

export async function sha256hex(bytes) {
  const subtle = globalThis.crypto && globalThis.crypto.subtle;
  if (!subtle) throw new RpcError("SHA-256 needs a secure page (https or localhost)");
  const d = new Uint8Array(await subtle.digest("SHA-256", bytes));
  return Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");
}

// Quote one argument for the device's shell: '...' or "...", no escape
// character, so an argument holding both kinds of quote cannot be written.
export function quote(arg) {
  if (arg && /^[A-Za-z0-9_./@%+=:,-]+$/.test(arg)) return arg;
  if (!arg.includes('"')) return `"${arg}"`;
  if (!arg.includes("'")) return `'${arg}'`;
  throw new RpcError(`${arg} holds both kinds of quote, which the device's shell cannot be given`);
}

export function joinPath(dir, name) {
  if (name.startsWith("/")) return name;
  return (dir.endsWith("/") ? dir : dir + "/") + name;
}

export function parentPath(path) {
  const p = path.replace(/\/+$/, "");
  const i = p.lastIndexOf("/");
  return i <= 0 ? "/" : p.slice(0, i);
}

// --- frames ------------------------------------------------------------------

export class Frame {
  constructor(bytes) {
    this.raw = bytes;
    const text = dec.decode(bytes);
    const sp = text.indexOf(" ");
    this.type = sp < 0 ? text : text.slice(0, sp);
    this.rest = sp < 0 ? "" : text.slice(sp + 1);
  }
  get final() { return this.type === "OK" || this.type === "ERR"; }
  kv() {
    const out = {};
    for (const word of this.rest.split(" ")) {
      const eq = word.indexOf("=");
      if (eq > 0) out[word.slice(0, eq)] = word.slice(eq + 1);
    }
    return out;
  }
  error() {
    const sp = this.rest.indexOf(" ");
    return sp < 0 ? new LinkError(this.rest || "?", "") : new LinkError(this.rest.slice(0, sp), this.rest.slice(sp + 1));
  }
}

export function parseHello(frame) {
  const info = frame.kv();
  if (info.cwd) {
    try { info.cwd = decodeURIComponent(info.cwd); } catch (e) { /* leave it as sent */ }
  }
  return info;
}

function noiseText(bytes) {
  let t = stripAnsi(dec.decode(bytes)).replace(/\r+$/, "");
  if (t.includes("\r")) t = t.split("\r").pop();
  return t;
}

// Splits the stream into frames and everything else. A frame is found
// wherever 0x1E appears, not only at the start of a line, and a trailing
// carriage return is dropped: the SDK's console may end lines with CRLF.
export class FrameReader {
  constructor(stream, onNoise) {
    this.s = stream;
    this.onNoise = onNoise;
    this.buf = new Uint8Array(0);
  }

  feed(bytes) { this.buf = concat(this.buf, bytes); }

  _line() {
    const i = this.buf.indexOf(0x0a);
    if (i < 0) return null;
    const line = this.buf.slice(0, i);
    this.buf = this.buf.slice(i + 1);
    return line;
  }

  // The next frame, or null after `ms` (null: wait for ever).
  async next(ms) {
    const end = ms == null ? null : Date.now() + ms;
    for (;;) {
      let line;
      while ((line = this._line()) !== null) {
        const at = line.indexOf(RS);
        if (at < 0) {
          if (this.onNoise && line.some((b) => b !== 0x0d)) this.onNoise(noiseText(line));
          continue;
        }
        if (at > 0 && this.onNoise) this.onNoise(noiseText(line.slice(0, at)));
        let body = line.slice(at + 1);
        if (body.length && body[body.length - 1] === 0x0d) body = body.slice(0, -1);
        return new Frame(body);
      }
      let wait = 500;
      if (end !== null) {
        wait = end - Date.now();
        if (wait <= 0) return null;
        wait = Math.min(wait, 500);
      }
      const data = await this.s.read(wait);
      if (data && data.length) this.feed(data);
    }
  }
}

// --- the client --------------------------------------------------------------

export class LinkClient {
  constructor(stream, { reader, onNoise, timing } = {}) {
    this.s = stream;
    this.r = reader || new FrameReader(stream, onNoise);
    this.t = { ...Timing, ...(timing || {}) };
    this.hello = {};
    this.busy = null;
    this.dropped = false;
    this._chain = Promise.resolve();
  }

  get chunk() {
    const n = parseInt(this.hello.chunk || DEFAULT_CHUNK, 10);
    return n > 0 ? Math.min(n, DEFAULT_CHUNK) : DEFAULT_CHUNK;
  }

  get execMax() { return parseInt(this.hello.exec_max || "0", 10); }

  setHello(frame) { this.hello = parseHello(frame); return this.hello; }

  // Requests queue behind one another rather than interleave on the wire.
  _serial(fn) {
    const run = this._chain.then(fn);
    this._chain = run.catch(() => {});
    return run;
  }

  static encode(line) {
    if (/[\r\n]/.test(line)) throw new RpcError("a request cannot contain a line break");
    const data = enc.encode(line + "\n");
    if (data.length > LINE_MAX) throw new RpcError(`the request is ${data.length} bytes; the device takes at most ${LINE_MAX}`);
    return data;
  }

  async frame(ms) {
    const f = await this.r.next(ms);
    if (!f) throw new TimeoutError(`no answer from the device in ${Math.round(ms / 1000)} s`);
    return f;
  }

  async final(ms, keep) {
    for (;;) {
      const f = await this.frame(ms);
      if (f.final) return f;
      if (keep) keep.push(f);
    }
  }

  // Run fn with `kind` in flight; on any failure while it is still in flight,
  // put the device back to waiting for a request first.
  async _guard(kind, fn) {
    if (this.dropped) throw new RpcError("link mode has ended; connect again");
    this.busy = kind;
    try {
      return await fn();
    } catch (e) {
      if (this.busy) {
        const k = this.busy;
        this.busy = null;
        try { await this.abort(k); } catch (e2) { /* the first error is the one to report */ }
      }
      throw e;
    } finally {
      this.busy = null;
    }
  }

  async abort(kind) {
    if (kind === "put" || kind === "get") await this.s.write(enc.encode("CANCEL\n"));
    else if (kind === "exec") await this.s.write(new Uint8Array([3]));
    try { await this.final(this.t.abort); } catch (e) { /* resync decides */ }
    await this.resync();
  }

  // A HELLO request is answered by a HELLO frame and then OK, so anything
  // stale before it is dropped. No HELLO means link mode has ended.
  async resync(ms) {
    const end = Date.now() + (ms || this.t.bye * 2);
    await this.s.write(enc.encode("HELLO\n"));
    let seen = false;
    for (;;) {
      const left = end - Date.now();
      const f = left > 0 ? await this.r.next(left) : null;
      if (!f) { this.dropped = true; return false; }
      if (f.type === "HELLO") { this.setHello(f); seen = true; }
      else if (seen && f.final) return true;
    }
  }

  async request(line, ms) {
    const data = LinkClient.encode(line);
    return this._serial(() => this._guard("req", async () => {
      await this.s.write(data);
      const keep = [];
      const f = await this.final(ms || this.t.request, keep);
      this.busy = null;
      if (f.type === "ERR") throw f.error();
      return { final: f, frames: keep };
    }));
  }

  async ping() { await this.request("PING"); }

  async helloRequest() {
    const { frames } = await this.request("HELLO");
    const h = frames.find((f) => f.type === "HELLO");
    if (!h) throw new RpcError("HELLO was answered without a HELLO frame");
    return this.setHello(h);
  }

  async ls(path) {
    const { frames } = await this.request("LS " + (path || "."));
    return frames.filter((f) => f.type === "E").map((f) => {
      const a = f.rest.indexOf(" ");
      const b = f.rest.indexOf(" ", a + 1);
      return { type: f.rest.slice(0, a), size: parseInt(f.rest.slice(a + 1, b), 10), name: f.rest.slice(b + 1) };
    });
  }

  async stat(path) {
    try {
      const { final } = await this.request("STAT " + path);
      const kv = final.kv();
      return { type: kv.type, size: parseInt(kv.size || "0", 10) };
    } catch (e) {
      if (e instanceof LinkError && e.code === "nofile") return null;
      throw e;
    }
  }

  // Run a shell line. After `timeout` ms, or when `signal` is aborted, a
  // Ctrl+C is sent and the result comes back with stopped set.
  async exec(line, { timeout, signal } = {}) {
    const data = LinkClient.encode("EXEC " + line);
    return this._serial(() => this._guard("exec", async () => {
      await this.s.write(data);
      const chunks = [];
      let stopped = false;
      let deadline = timeout ? Date.now() + timeout : null;
      for (;;) {
        let wait = 500;
        if (deadline !== null) wait = Math.max(0, Math.min(500, deadline - Date.now()));
        const f = await this.r.next(wait);
        if (!f) {
          const due = (deadline !== null && Date.now() >= deadline) || (signal && signal.aborted);
          if (due && !stopped) {
            await this.s.write(new Uint8Array([3]));
            stopped = true;
            deadline = Date.now() + this.t.abort;
          } else if (due && stopped) {
            throw new TimeoutError("the command did not stop after Ctrl+C");
          }
          continue;
        }
        if (f.type === "OUT") chunks.push(b64decode(f.rest));
        else if (f.type === "OK") {
          this.busy = null;
          const kv = f.kv();
          const output = chunks.reduce(concat, new Uint8Array(0));
          return {
            rc: parseInt(kv.rc || "1", 10), output, text: dec.decode(output),
            truncated: kv.truncated === "1", stopped,
          };
        } else if (f.type === "ERR") {
          this.busy = null;
          throw f.error();
        }
      }
    }));
  }

  // Write `data` to `path`; resolves to the digest the device confirmed.
  put(data, path, { progress, signal } = {}) {
    return this._serial(async () => {
      const digest = await sha256hex(data);
      const req = LinkClient.encode(`PUT ${data.length} ${digest} ${path}`);
      return this._guard("put", async () => {
        await this.s.write(req);
        let f = await this.frame(this.t.transfer);
        while (!(f.type === "READY" || f.final)) f = await this.frame(this.t.transfer);
        if (f.type === "ERR") { this.busy = null; throw f.error(); }
        if (f.type === "OK") throw new RpcError("PUT was answered OK before READY");
        const chunk = Math.max(1, Math.min(parseInt(f.kv().chunk || this.chunk, 10) || this.chunk, this.chunk));
        let sent = 0;
        if (progress) progress(0, data.length);
        while (sent < data.length) {
          if (signal && signal.aborted) throw new CancelledError();
          const piece = data.subarray(sent, sent + chunk);
          await this.s.write(enc.encode("D " + b64encode(piece) + "\n"));
          sent += piece.length;
          for (;;) {
            f = await this.frame(this.t.transfer);
            if (f.type === "ACK") break;
            if (f.type === "ERR") { this.busy = null; throw f.error(); }
            if (f.type === "OK") throw new RpcError(`PUT ended early, after ${sent} bytes`);
          }
          const acked = parseInt(f.rest, 10);
          if (acked !== sent) throw new RpcError(`the device acknowledged ${acked} bytes, ${sent} were sent`);
          if (progress) progress(sent, data.length);
        }
        f = await this.final(this.t.transfer);
        this.busy = null;
        if (f.type === "ERR") throw f.error();
        const got = (f.kv().sha || "").toLowerCase();
        if (got !== digest) throw new DigestError(`the device reports sha256 ${got || "(none)"}, expected ${digest}`);
        return digest;
      });
    });
  }

  // Read a file; resolves to its bytes, checked against the device's digest.
  async get(path, { progress, signal } = {}) {
    const req = LinkClient.encode("GET " + path);
    return this._serial(() => this._guard("get", async () => {
      await this.s.write(req);
      let f = await this.frame(this.t.transfer);
      while (!(f.type === "SIZE" || f.final)) f = await this.frame(this.t.transfer);
      if (f.type === "ERR") { this.busy = null; throw f.error(); }
      if (f.type === "OK") throw new RpcError("GET was answered OK before SIZE");
      const size = parseInt(f.rest, 10);
      const parts = [];
      let got = 0;
      if (progress) progress(0, size);
      for (;;) {
        f = await this.frame(this.t.transfer);
        if (f.type === "D") {
          const b = b64decode(f.rest);
          parts.push(b);
          got += b.length;
          if (progress) progress(got, size);
          if (signal && signal.aborted) throw new CancelledError();
          await this.s.write(enc.encode("A\n"));
        } else if (f.type === "ERR") {
          this.busy = null;
          throw f.error();
        } else if (f.type === "OK") {
          this.busy = null;
          break;
        }
      }
      const data = new Uint8Array(got);
      let off = 0;
      for (const p of parts) { data.set(p, off); off += p.length; }
      const want = (f.kv().sha || "").toLowerCase();
      if (data.length !== size) throw new DigestError(`${path}: ${data.length} bytes arrived, the device said ${size}`);
      const have = await sha256hex(data);
      if (want !== have) throw new DigestError(`${path}: the device sent sha256 ${want || "(none)"}, what arrived is ${have}`);
      return data;
    }));
  }

  // Leave link mode. Resolves true when the device said OK.
  bye() {
    return this._serial(async () => {
      if (this.dropped) return false;
      try {
        await this.s.write(enc.encode("BYE\n"));
        const end = Date.now() + this.t.bye;
        for (;;) {
          const left = end - Date.now();
          if (left <= 0) return false;
          const f = await this.r.next(left);
          if (!f) return false;
          if (f.final) { this.dropped = true; return f.type === "OK"; }
        }
      } catch (e) {
        return false;
      }
    });
  }
}

// --- getting into link mode ----------------------------------------------------

const SHELL_PROMPT = /\S+@\S+:.*>\s*$/;

function lastLine(text) {
  return text.replace(/\r\n/g, "\n").split("\n").pop().split("\r").pop();
}

// What the console is showing, from what it printed after a Ctrl+C:
// "shell", "login", "password", "link", "other" or null (nothing at all).
export function classify(bytes) {
  let text = stripAnsi(dec.decode(bytes));
  const rs = text.lastIndexOf("\x1e");
  if (rs >= 0) {
    let tail = text.slice(rs);
    const nl = tail.indexOf("\n");
    tail = nl >= 0 ? tail.slice(nl + 1) : "";
    if (!SHELL_PROMPT.test(lastLine(tail))) return "link";
    text = tail;
  }
  const last = lastLine(text);
  if (last.includes("Username")) return "login";
  if (last.includes("Password")) return "password";
  if (SHELL_PROMPT.test(last) || text.includes("^C")) return "shell";
  return text.trim() ? "other" : null;
}

async function collect(stream, ms) {
  let buf = new Uint8Array(0);
  const end = Date.now() + ms;
  for (;;) {
    const left = end - Date.now();
    if (left <= 0) return buf;
    const d = await stream.read(Math.min(left, 100));
    if (d && d.length) buf = concat(buf, d);
  }
}

async function waitFor(stream, patterns, ms) {
  let buf = new Uint8Array(0);
  const end = Date.now() + ms;
  while (Date.now() < end) {
    const d = await stream.read(100);
    if (d && d.length) buf = concat(buf, d);
    const text = stripAnsi(dec.decode(buf));
    for (let i = 0; i < patterns.length; i++) if (patterns[i].test(text)) return i;
  }
  return -1;
}

async function typeLine(stream, text) {
  for (const b of enc.encode(text)) await stream.write(new Uint8Array([b]));
  await stream.write(enc.encode("\r"));
}

async function login(stream, user, password, t) {
  if (!user) {
    throw new LoginRequired("The device is at a login prompt. Enter an account on the Connect tab, or log in from the Terminal.");
  }
  await typeLine(stream, user);
  const i = await waitFor(stream, [/not found/, /Password[^\n]*$/m, /Welcome|No password required/], 5000);
  if (i === 0) throw new RpcError(`The device has no account called '${user}'.`);
  if (i === 1) {
    const pw = typeof password === "function" ? await password(user) : password;
    if (!pw) throw new LoginRequired(`The account '${user}' needs a password.`);
    await typeLine(stream, pw);
    const j = await waitFor(stream, [/Incorrect password|Too many/, /Welcome/], 12000);
    if (j !== 1) throw new RpcError(`The device refused the password for '${user}'.`);
  } else if (i < 0) {
    throw new RpcError("No answer to the username.");
  }
  await collect(stream, t.settle);
}

async function awaitHello(stream, t, onNoise) {
  let buf = new Uint8Array(0);
  const end = Date.now() + t.enter;
  while (Date.now() < end) {
    const d = await stream.read(100);
    if (d && d.length) buf = concat(buf, d);
    const at = indexOfSeq(buf, enc.encode("\x1eHELLO"));
    if (at >= 0) {
      const nl = buf.indexOf(0x0a, at);
      if (nl < 0) continue;
      let body = buf.slice(at + 1, nl);
      if (body.length && body[body.length - 1] === 0x0d) body = body.slice(0, -1);
      const reader = new FrameReader(stream, onNoise);
      reader.feed(buf.slice(nl + 1));
      const link = new LinkClient(stream, { reader, timing: t });
      link.setHello(new Frame(body));
      return { what: "hello", link };
    }
    const text = stripAnsi(dec.decode(buf));
    if (text.includes("'link' is not a command")) { await collect(stream, t.settle); return { what: "old" }; }
    if (lastLine(text).includes("Username")) return { what: "login" };
    const rs = buf.indexOf(RS);
    if (rs >= 0 && buf.indexOf(0x0a, rs) >= 0) return { what: "link" };
  }
  return { what: null };
}

function indexOfSeq(hay, needle) {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}

// From whatever the console is doing to link mode. Resolves to
// { mode: "link", link } or, on firmware older than link mode, { mode: "legacy" }
// with the device left at its prompt. `password` may be a function that asks.
export async function enterLink(stream, { user, password, timing, onNoise } = {}) {
  const t = { ...Timing, ...(timing || {}) };
  if (stream.discard) stream.discard();
  for (let attempt = 0; attempt < 4; attempt++) {
    await stream.write(new Uint8Array([3]));
    let seen = await collect(stream, t.settle);
    let state = classify(seen);
    if (state === null) {
      // A login prompt ignores Ctrl+C; an empty line is refused there without
      // being counted as an attempt.
      await stream.write(enc.encode("\r"));
      seen = await collect(stream, t.settle * 2);
      state = classify(seen);
    }
    if (state === "password") {
      throw new LoginRequired("The device is waiting for a password that something else started. Finish or abandon that login in the Terminal.");
    }
    if (state === "login") { await login(stream, user, password, t); continue; }
    if (state === "link") {
      const link = new LinkClient(stream, { onNoise, timing: t });
      if (await link.resync()) return { mode: "link", link };
      continue;
    }
    await stream.write(enc.encode("link\r"));
    const r = await awaitHello(stream, t, onNoise);
    if (r.what === "hello") return { mode: "link", link: r.link };
    if (r.what === "old") return { mode: "legacy" };
    if (r.what === "login") await login(stream, user, password, t);
  }
  throw new RpcError("The device did not answer. Check it is running and that nothing else has the port open.");
}

// Copy a package to /tmp, `pkg install` it, and remove the copy again.
export async function installPackage(link, data, name, { progress, signal } = {}) {
  const base = name.replace(/\.app$/i, "").replace(/[^A-Za-z0-9_.-]/g, "_").slice(0, 40) || "package";
  const tmp = `/tmp/${base}.app`;
  await link.put(data, tmp, { progress, signal });
  try {
    return await link.exec("pkg install " + quote(tmp));
  } finally {
    try { await link.exec("rm " + quote(tmp)); } catch (e) { /* it is in /tmp, emptied at boot */ }
  }
}
