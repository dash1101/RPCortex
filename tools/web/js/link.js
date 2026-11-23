// The link protocol client (LINK.md, protocols 1 to 3), for the browser and for Node.
//
// No DOM here. It talks to a byte stream with two methods:
//
//   write(Uint8Array) -> Promise
//   read(ms)          -> Promise<Uint8Array>   (empty after ms with nothing)
//
// One request at a time, read to its final OK or ERR before the next is sent;
// calls made while one is running wait their turn. Lines that are not frames
// are background output from the device and go to onNoise, never to a parser.
//
// Protocol 3 adds the network bridge: a board with no network of its own asks
// the host to fetch a URL in the middle of a command. Bridge (below) answers,
// with the browser's fetch(); TermBridge does the same for the terminal.

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
    this.pending = [];          // frames handed back to be read again (unread)
  }

  feed(bytes) { this.buf = concat(this.buf, bytes); }

  unread(frames) { this.pending.unshift(...frames); }

  _line() {
    const i = this.buf.indexOf(0x0a);
    if (i < 0) return null;
    const line = this.buf.slice(0, i);
    this.buf = this.buf.slice(i + 1);
    return line;
  }

  // The next frame, or null after `ms` (null: wait for ever).
  async next(ms) {
    if (this.pending.length) return this.pending.shift();
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
    // For sudo (protocol 2): async (user, wrongBefore) => the account's
    // password, or null to give up. Asked when the device first says a request
    // needs sudo; the password is kept for the rest of the connection.
    this.sudoPassword = null;
    this._sudoKnown = null;
    this.bridge = null;           // answers FETCH frames (protocol 3), when set
    this.bridged = false;
  }

  get proto() { return parseInt(this.hello.proto || "1", 10) || 1; }

  // The firmware has the bridge and no network of its own (HELLO bridge=1).
  get canBridge() { return this.proto >= 3 && this.hello.bridge === "1"; }

  // BRIDGE on: this host will answer the device's fetches. The offer outlives
  // BYE (it belongs to the USB connection), which is what lets the terminal
  // keep answering after link mode.
  async bridgeOn() {
    if (!this.canBridge) return false;
    const { final } = await this.request("BRIDGE on");
    this.bridged = final.kv().bridge === "on";
    return this.bridged;
  }

  // The next frame, answering any FETCH on the way when there is a bridge.
  async nextFrame(ms, interrupt) {
    for (;;) {
      const f = await this.r.next(ms);
      if (!f || f.type !== "FETCH" || !this.bridge) return f;
      const left = await this.bridge.serve(f, (t) => this.r.next(t), (b) => this.s.write(b), interrupt);
      this.r.unread(left);
    }
  }
  get hasSudo() { return this.proto >= 2; }

  // `line` as it has to be sent to run with sudo on this firmware.
  sudoLine(line) { return this.hasSudo ? "sudo " + line : line; }

  // AUTH: prove the signed-in account's password. Resolves to the seconds the
  // device says it counts for; a wrong one rejects with LinkError "auth".
  async auth(password) {
    const { final } = await this.request("AUTH " + b64encode(enc.encode(password)));
    return parseInt(final.kv().grace || "0", 10);
  }

  // AUTH, asking for the password if need be: three tries when it is typed.
  async authenticate() {
    if (!this.hasSudo) return false;
    const user = this.hello.user || "this account";
    let wrong = false;
    for (let i = 0; i < 3; i++) {
      const known = this._sudoKnown;
      const pw = known || (this.sudoPassword ? await this.sudoPassword(user, wrong) : null);
      if (!pw) throw new RpcError(`This needs sudo on the device, which asks for ${user}'s password.`);
      try {
        await this.auth(pw);
        this._sudoKnown = pw;
        return true;
      } catch (e) {
        if (!(e instanceof LinkError) || e.code !== "auth") throw e;
        this._sudoKnown = null;
        wrong = true;
        if (known) continue;
      }
    }
    throw new RpcError(`Three wrong passwords for ${user}; nothing was done.`);
  }

  // Run fn; if the device says it needs sudo, AUTH and run it once more.
  async _withSudo(fn) {
    try {
      return await fn();
    } catch (e) {
      if (!(e instanceof LinkError) || e.code !== "sudo" || !this.hasSudo) throw e;
    }
    await this.authenticate();
    return fn();
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
    const f = await this.nextFrame(ms);
    if (!f) throw new TimeoutError(`no answer from the device in ${Math.round(ms / 1000)} s`);
    return f;
  }

  async final(ms, keep) {
    for (;;) {
      const f = await this.frame(ms);
      if (f.final) return f;
      if (f.type === "BYE") {
        // The device left link mode (Ctrl+C on its console, or the port was
        // reopened) instead of answering.
        this.dropped = true;
        throw new RpcError("the device left link mode before answering; reconnect");
      }
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

  // LS, STAT and GET of the files holding passwords and keys need sudo on
  // protocol 2 firmware, like a PUT into /os: AUTH, then the same request.
  ls(path) { return this._withSudo(() => this._ls(path)); }

  async _ls(path) {
    const { frames } = await this.request("LS " + (path || "."));
    return frames.filter((f) => f.type === "E").map((f) => {
      const a = f.rest.indexOf(" ");
      const b = f.rest.indexOf(" ", a + 1);
      return { type: f.rest.slice(0, a), size: parseInt(f.rest.slice(a + 1, b), 10), name: f.rest.slice(b + 1) };
    });
  }

  stat(path) { return this._withSudo(() => this._stat(path)); }

  async _stat(path) {
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
  // Ctrl+C is sent and the result comes back with stopped set. A line the
  // device says needs sudo is run again after AUTH.
  exec(line, opts = {}) { return this._withSudo(() => this._exec(line, opts)); }

  async _exec(line, { timeout, signal } = {}) {
    const data = LinkClient.encode("EXEC " + line);
    return this._serial(() => this._guard("exec", async () => {
      await this.s.write(data);
      const chunks = [];
      let stopped = false;
      let deadline = timeout ? Date.now() + timeout : null;
      // A deadline or a Stop in the middle of a fetch: one Ctrl+C, which the
      // device turns into FSTOP as it would one typed at its console.
      let sent = false;
      const due = () => {
        if (sent) return false;
        const late = deadline !== null && Date.now() >= deadline;
        const asked = signal && signal.aborted;
        if (late || asked) { sent = true; return true; }
        return false;
      };
      for (;;) {
        let wait = 500;
        if (deadline !== null) wait = Math.max(0, Math.min(500, deadline - Date.now()));
        const f = await this.nextFrame(wait, due);
        if (sent && !stopped) { stopped = true; deadline = Date.now() + this.t.abort; }
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

  // Write `data` to `path`; resolves to the digest the device confirmed. A
  // path the device says needs sudo (in /os or /etc) is tried again after AUTH.
  put(data, path, opts = {}) { return this._withSudo(() => this._put(data, path, opts)); }

  _put(data, path, { progress, signal } = {}) {
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
  get(path, opts = {}) { return this._withSudo(() => this._get(path, opts)); }

  async _get(path, { progress, signal } = {}) {
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
export async function enterLink(stream, { user, password, timing, onNoise, bridge } = {}) {
  const r = await enterLinkMode(stream, { user, password, timing, onNoise });
  if (r.mode === "link" && bridge) {
    r.link.bridge = bridge;
    try { await r.link.bridgeOn(); } catch (e) { /* the commands still run, without a network */ }
  }
  return r;
}

async function enterLinkMode(stream, { user, password, timing, onNoise } = {}) {
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

// Copy a package to /tmp, `pkg install` it (with sudo where the firmware has
// it: installing changes the system), and remove the copy again.
export async function installPackage(link, data, name, { progress, signal } = {}) {
  const base = name.replace(/\.app$/i, "").replace(/[^A-Za-z0-9_.-]/g, "_").slice(0, 40) || "package";
  const tmp = `/tmp/${base}.app`;
  await link.put(data, tmp, { progress, signal });
  try {
    return await link.exec(link.sudoLine("pkg install " + quote(tmp)));
  } finally {
    try { await link.exec("rm " + quote(tmp)); } catch (e) { /* it is in /tmp, emptied at boot */ }
  }
}

// --- the network bridge (protocol 3) -------------------------------------------
//
// LINK.md, "The network bridge". The device asks; this fetches with the
// browser's own fetch() and streams the body back, one chunk per FACK. What the
// browser can fetch is narrower than what rpc can: a page may read only what the
// server allows it to (CORS). raw.githubusercontent.com does (the package
// repository, the packages and the update images published there); GitHub's
// release downloads (github.com/.../releases/download/...) do not, nor do most
// sites. rpc term has no such limit. And the browser takes only simple requests:
// GET and HEAD, with no headers of the device's own, since anything else asks the
// server first (a preflight) and most servers refuse.

export const BRIDGE_TYPES = ["FETCH", "FHDR", "FBODY", "FGO", "FACK", "FSTOP"];
export const BRIDGE_SHOWN = [...BRIDGE_TYPES, "BRIDGE"];  // kept off a terminal's screen
export const BRIDGE_OFFER = new Uint8Array([0x1e, 0x1f]);  // a terminal's offer
const BRIDGE_CHUNK = 768;
const BRIDGE_WAIT_EVERY = 2000;
const BRIDGE_ACK_WAIT = 20000;
const BRIDGE_COLLECT = 5000;
const PASS = ["content-type", "etag", "last-modified"];

export class BridgeRefused extends RpcError {
  constructor(code, message) { super(message); this.name = "BridgeRefused"; this.code = code; }
}

function frameId(f) { const n = parseInt(f.rest, 10); return Number.isFinite(n) ? n : null; }
function afterId(f) { const i = f.rest.indexOf(" "); return i < 0 ? "" : f.rest.slice(i + 1); }

export class Bridge {
  // allow: host names (".example.com" covers its subdomains); empty allows any.
  // maxBytes: the most one fetch may bring in. log(text): a line per request.
  constructor({ allow = [], maxBytes = 8 * 1024 * 1024, log = null, fetchFn = null } = {}) {
    this.allow = allow.map((a) => a.trim().toLowerCase()).filter(Boolean);
    this.maxBytes = maxBytes;
    this.log = log || (() => {});
    this.fetchFn = fetchFn || ((...a) => globalThis.fetch(...a));
    this.busy = false;
    this.served = [];
  }

  check(url) {
    let u;
    try { u = new URL(url); } catch (e) { throw new BridgeRefused("refused", "not a URL"); }
    if (u.protocol !== "http:" && u.protocol !== "https:") {
      throw new BridgeRefused("refused", `only http and https are bridged, not ${u.protocol.replace(":", "")}`);
    }
    const host = u.hostname.toLowerCase();
    if (this.allow.length && !this.allow.some((a) => a === "*" || a === host ||
        (a.startsWith(".") && host.endsWith(a)) || (a.startsWith("*.") && host.endsWith(a.slice(1))))) {
      throw new BridgeRefused("refused", `${host} is not on this page's bridge allowlist`);
    }
  }

  // Answer the FETCH frame `first`. next(ms) gives the device's next frame or
  // null; write(bytes) sends to it; interrupt() true sends a Ctrl+C. Resolves
  // to the frames read on the way that were not this fetch's.
  async serve(first, next, write, interrupt) {
    this.busy = true;
    const nx = interrupt ? async (ms) => { if (interrupt()) await write(new Uint8Array([3])); return next(ms); } : next;
    try { return await this._serve(first, nx, write); } finally { this.busy = false; }
  }

  async _serve(first, next, write) {
    const left = [];
    const id = frameId(first);
    const rest = afterId(first);
    const sp = rest.indexOf(" ");
    const method = sp < 0 ? "" : rest.slice(0, sp);
    const url = sp < 0 ? "" : rest.slice(sp + 1);
    if (id === null || !method || !url) return left;
    const headers = [];
    let chunk = BRIDGE_CHUNK;
    for (;;) {
      const f = await next(BRIDGE_COLLECT);
      if (!f) return left;
      if (BRIDGE_TYPES.includes(f.type) && f.type !== "FETCH" && frameId(f) === id) {
        if (f.type === "FHDR") headers.push(dec.decode(b64decode(afterId(f))));
        else if (f.type === "FGO") { chunk = Math.max(1, Math.min(parseInt(f.kv().chunk || BRIDGE_CHUNK, 10) || BRIDGE_CHUNK, BRIDGE_CHUNK)); break; }
        // FBODY: the browser sends no body (simple requests only).
      } else { left.push(f); if (f.type === "FETCH") return left; }
    }
    return left.concat(await this._answer(id, method, url, headers, chunk, next, write));
  }

  async _answer(id, method, url, headers, chunk, next, write) {
    const left = [];
    const say = (text) => write(enc.encode("\x1e" + text + "\n"));
    const t0 = Date.now();
    await say(`FWAIT ${id}`);
    const done = async (code, msg, status, got = 0) => {
      if (code) {
        await say(`FDONE ${id} err ${code} ${msg}`);
        this.log(`bridge: ${method} ${url} failed: ${msg}`);
        this.served.push([method, url, code, got]);
      } else {
        await say(`FDONE ${id} ok`);
        this.log(`bridge: ${method} ${url} -> ${status}, ${got} bytes in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
        this.served.push([method, url, status, got]);
      }
    };
    try {
      this.check(url);
      if (method !== "GET" && method !== "HEAD") {
        throw new BridgeRefused("refused", `the browser bridges only GET and HEAD, not ${method}; rpc term bridges the rest`);
      }
    } catch (e) {
      if (!(e instanceof BridgeRefused)) throw e;
      this.log(`bridge: ${method} ${url} refused: ${e.message}`);
      await say(`FDONE ${id} err ${e.code} ${e.message}`);
      this.served.push([method, url, e.code, 0]);
      return left;
    }
    this.log(`bridge: ${method} ${url}`);

    const ctl = new AbortController();
    // A simple request: no headers of the device's own and no cookies.
    const init = { method, redirect: "follow", credentials: "omit", cache: "no-store", signal: ctl.signal };
    // The response, with FWAIT every couple of seconds while it is coming, and
    // the device's frames read meanwhile so that its FSTOP is seen.
    let settled = null;
    const pending = this.fetchFn(url, init).then((r) => { settled = { r }; }, (e) => { settled = { e }; });
    let lastWait = Date.now();
    while (!settled) {
      const f = await next(20);
      if (f && f.type === "FSTOP" && frameId(f) === id) { ctl.abort(); await done("cancelled", "stopped by the device"); return left; }
      if (f) { left.push(f); if (f.final) { ctl.abort(); return left; } }
      await Promise.race([pending, new Promise((r) => setTimeout(r, 30))]);
      if (Date.now() - lastWait >= BRIDGE_WAIT_EVERY) { await say(`FWAIT ${id}`); lastWait = Date.now(); }
    }
    if (settled.e) {
      await done("net", "the browser could not fetch it (most sites do not let a page read them: CORS); rpc term can");
      return left;
    }
    const resp = settled.r;
    // A redirect the browser followed may have gone somewhere the allowlist
    // does not cover; it cannot be stopped part way, but it is not passed on.
    try { if (resp.url) this.check(resp.url); } catch (e) {
      ctl.abort();
      await done("refused", e.message);
      return left;
    }
    // No len: the browser undoes gzip itself and Content-Length is the size
    // before that, so the body ends at FDONE instead.
    await say(`FHEAD ${id} ${resp.status}`);
    for (const name of PASS) {
      const v = resp.headers.get(name);
      if (v && !/[\r\n]/.test(v)) {
        const line = (name === "etag" ? "ETag" : name.split("-").map((p) => p[0].toUpperCase() + p.slice(1)).join("-")) + ": " + v;
        if (line.length < 200) await say(`FHDR ${id} ${b64encode(enc.encode(line))}`);
      }
    }
    let got = 0;
    if (method === "HEAD" || !resp.body) { await done(null, null, resp.status, 0); return left; }
    const reader = resp.body.getReader();
    let buf = new Uint8Array(0);
    let ended = false;
    try {
      for (;;) {
        while (!ended && buf.length < chunk) {
          const { value, done: end } = await reader.read();
          if (end) ended = true;
          else if (value && value.length) buf = concat(buf, value);
        }
        if (!buf.length && ended) { await done(null, null, resp.status, got); return left; }
        if (got + Math.min(chunk, buf.length) > this.maxBytes) {
          ctl.abort();
          await done("toobig", `over this page's ${Math.round(this.maxBytes / 1048576)} MB bridge limit`, null, got);
          return left;
        }
        const piece = buf.subarray(0, chunk);
        buf = buf.slice(piece.length);
        await say(`FDATA ${id} ${b64encode(piece)}`);
        got += piece.length;
        // One chunk in flight: nothing more until the device has dealt with it.
        const end = Date.now() + BRIDGE_ACK_WAIT;
        for (;;) {
          const f = await next(Math.max(0, end - Date.now()));
          if (!f) { ctl.abort(); this.log(`bridge: ${url}: the device stopped acknowledging`); return left; }
          if (f.type === "FSTOP" && frameId(f) === id) { ctl.abort(); await done("cancelled", "stopped by the device", null, got); return left; }
          if (f.type === "FACK" && frameId(f) === id) break;
          left.push(f);
          if (f.final) { ctl.abort(); return left; }
        }
      }
    } catch (e) {
      await done("net", `the download broke off: ${e.message || e}`, null, got);
      return left;
    }
  }
}

// The terminal's half: what the device prints goes through feed(), which keeps
// the bridge's frames off the screen and answers them; keys go through key(),
// which holds them back while a fetch runs, all but Ctrl+C.
export class TermBridge {
  constructor(bridge, write, { onNote = null } = {}) {
    this.bridge = bridge;
    this.write = write;
    this.onNote = onNote || (() => {});
    this.frame = null;            // a 0x1E line still arriving
    this.q = [];
    this.waiters = [];
    this.fetching = false;
    this.held = [];
    this.offered = false;
    this.chain = Promise.resolve();
    this.running = false;
  }

  // Bytes from the device in; the bytes to show out.
  feed(bytes) {
    const show = [];
    let i = 0;
    while (i < bytes.length) {
      if (this.frame === null) {
        const j = bytes.indexOf(0x1e, i);
        if (j < 0) { show.push(bytes.subarray(i)); break; }
        show.push(bytes.subarray(i, j));
        this.frame = new Uint8Array(0);
        i = j + 1;
        continue;
      }
      const j = bytes.indexOf(0x0a, i);
      if (j < 0) {
        this.frame = concat(this.frame, bytes.subarray(i));
        if (this.frame.length > 8192) { show.push(concat(new Uint8Array([0x1e]), this.frame)); this.frame = null; }
        break;
      }
      const raw = concat(this.frame, bytes.subarray(i, j));
      this.frame = null;
      i = j + 1;
      const body = raw.length && raw[raw.length - 1] === 0x0d ? raw.subarray(0, raw.length - 1) : raw;
      const f = new Frame(body);
      if (!BRIDGE_SHOWN.includes(f.type)) {
        show.push(new Uint8Array([0x1e]), raw, new Uint8Array([0x0a]));
      } else if (f.type === "BRIDGE") {
        if (!this.offered) { this.offered = true; this.onNote("network bridge on: commands on the device can use this computer's connection"); }
      } else {
        if (f.type === "FETCH") this.fetching = true;
        this._push(f);
      }
    }
    return show.reduce(concat, new Uint8Array(0));
  }

  _push(f) {
    this.q.push(f);
    const w = this.waiters; this.waiters = [];
    for (const r of w) r();
    if (!this.running) this._work();
  }

  async _next(ms) {
    const end = Date.now() + (ms == null ? 3600000 : ms);
    while (!this.q.length) {
      const left = end - Date.now();
      if (left <= 0) return null;
      await new Promise((r) => { const t = setTimeout(r, left); this.waiters.push(() => { clearTimeout(t); r(); }); });
    }
    return this.q.shift();
  }

  async _work() {
    this.running = true;
    try {
      while (this.q.length) {
        const f = this.q.shift();
        if (f.type !== "FETCH") continue;
        this.fetching = true;
        try {
          const left = await this.bridge.serve(f, (ms) => this._next(ms), (b) => this.write(b));
          for (const x of left) if (x.type === "FETCH") this.q.push(x);
        } catch (e) {
          this.onNote(`bridge: ${e.message || e}`);
        } finally {
          await this._release();
        }
      }
    } finally {
      this.running = false;
    }
  }

  async _release() {
    this.fetching = false;
    if (this.held.length) {
      const h = this.held.reduce(concat, new Uint8Array(0));
      this.held = [];
      await this.write(h);
    }
  }

  // Keys from the person at the terminal.
  async key(bytes) {
    if (this.fetching) {
      if (bytes.includes(3)) await this.write(new Uint8Array([3]));   // Ctrl+C stops the fetch
      this.held.push(bytes.filter((b) => b !== 3));
      return;
    }
    await this.write(bytes);
  }

  // Offer the bridge: harmless to any firmware that does not have it.
  offer() { return this.write(BRIDGE_OFFER); }
}
