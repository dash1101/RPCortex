// A pretend RPCortex v2 device, for the tests and for the page's demo mode
// (index.html?demo). It is the JavaScript twin of tools/rpc/tests/mockdev.py,
// smaller: the prompt and its echo, the login prompt, firmware without link
// mode, and link mode as os/shell/link.cpp implements it.

const enc = new TextEncoder();
const dec = new TextDecoder();

function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

export class ByteQueue {
  constructor() { this.buf = new Uint8Array(0); this.waiters = []; this.closed = false; }
  push(bytes) { this.buf = concat(this.buf, bytes); this._wake(); }
  unshift(bytes) { this.buf = concat(bytes, this.buf); this._wake(); }
  _wake() { const w = this.waiters; this.waiters = []; for (const f of w) f(); }
  close() { this.closed = true; this._wake(); }
  clear() { this.buf = new Uint8Array(0); }
  // Up to `max` bytes, waiting up to `ms` (null: for ever) for the first.
  async take(max, ms) {
    const end = ms == null ? null : Date.now() + ms;
    while (!this.buf.length && !this.closed) {
      const left = end == null ? null : end - Date.now();
      if (left != null && left <= 0) break;
      await new Promise((resolve) => {
        let timer = null;
        const wake = () => { if (timer) clearTimeout(timer); resolve(); };
        this.waiters.push(wake);
        if (left != null) {
          timer = setTimeout(() => {
            const i = this.waiters.indexOf(wake);
            if (i >= 0) this.waiters.splice(i, 1);
            resolve();
          }, left);
        }
      });
    }
    if (!this.buf.length && this.closed) throw new Error("closed");
    const n = Math.min(max, this.buf.length);
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
  }
}

const SYSTEM = ["/", "/os", "/os/pkg", "/etc", "/home", "/tmp", "/usb"];

function resolve(cwd, p) {
  const parts = (p.startsWith("/") ? p : cwd + "/" + p).split("/");
  const out = [];
  for (const s of parts) {
    if (!s || s === ".") continue;
    if (s === "..") out.pop(); else out.push(s);
  }
  return "/" + out.join("/");
}

function dirname(p) { const i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
function basename(p) { return p.slice(p.lastIndexOf("/") + 1); }

export function makeApp(name, ver = "1.0", size = 600) {
  const head = enc.encode(`RPCAPP ${name} ${ver}\n`);
  const out = new Uint8Array(Math.max(size, head.length));
  out.set(head, 0);
  for (let i = head.length; i < out.length; i++) out[i] = (i * 7) & 0xff;
  return out;
}

async function sha256hex(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(d, (b) => b.toString(16).padStart(2, "0")).join("");
}

function b64(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s);
}

export class MockDevice {
  constructor({ link = true, login = false, crlf = true, noise = 0, board = "pico2_w", user = "root",
                proto = 2 } = {}) {
    this.linkSupported = link;
    // Protocol 2 is the firmware with sudo (core/sudo.cpp): `pkg install` and
    // a PUT into /os or /etc need it, EXEC never asks for a password and says
    // ERR sudo instead, and AUTH starts the grace. 1 is the firmware before.
    this.proto = link ? proto : 1;
    this.admins = new Set(["root"]);
    this.grace = false;
    this.elevated = false;
    this.noprompt = false;
    this.sudoBlocked = false;
    this.auths = [];
    this.crlf = crlf;
    this.noise = noise;
    this.board = board;
    this.users = { root: "rootpw", guest: null };
    this.user = login ? null : user;
    this.cwd = "/";
    this.files = new Map();
    this.dirs = new Set([...SYSTEM, "/home/root", "/home/guest"]);
    this.installed = { greet: "1.0" };
    this.toHost = new ByteQueue();
    this.fromHost = new ByteQueue();
    this.inLink = false;
    this.requests = [];
    this.loginTyped = [];
    this.byes = 0;
    this.corruptGet = false;
    this.capture = null;
    this.hadError = false;
    this.onReboot = null;
    this.seed = 7;
    const put = (p, s) => this.files.set(p, typeof s === "string" ? enc.encode(s) : s);
    put("/os/registry.cfg", "System.Setup=true\n");
    put("/os/pkg/greet.app", makeApp("greet"));
    put("/etc/startup.cfg", "# startup\n");
    put("/home/root/notes.txt", "hello from the device\n");
    put("/usb/readme.txt", "drop files here\n");
  }

  rand() { this.seed = (this.seed * 1103515245 + 12345) & 0x7fffffff; return this.seed / 0x7fffffff; }

  // the host's end: write(bytes), read(ms), discard()
  stream() {
    return {
      write: async (b) => { this.fromHost.push(b); },
      read: async (ms) => { try { return await this.toHost.take(1 << 16, ms); } catch (e) { return new Uint8Array(0); } },
      discard: () => this.toHost.clear(),
    };
  }

  start() { this._main().catch(() => {}); return this; }
  stop() { this.toHost.close(); this.fromHost.close(); }

  emit(data) {
    let b = typeof data === "string" ? enc.encode(data) : data;
    if (this.crlf) {
      const out = [];
      for (let i = 0; i < b.length; i++) {
        if (b[i] === 0x0a && (i === 0 || b[i - 1] !== 0x0d)) out.push(0x0d);
        out.push(b[i]);
      }
      b = new Uint8Array(out);
    }
    this.toHost.push(b);
  }

  async key(ms) {
    const b = await this.fromHost.take(1, ms);
    return b.length ? b[0] : null;
  }

  out(text) {
    const b = typeof text === "string" ? enc.encode(text) : text;
    if (this.capture) this.capture.push(b); else this.emit(b);
  }
  tag(colour, sym, msg) {
    if (this.capture) this.out(`[${sym}] ${msg}\n`);
    else this.emit(`\x1b[${colour}m[\x1b[37m${sym}\x1b[${colour}m]\x1b[0m ${msg}\n`);
  }
  ok(m) { this.tag(36, "@", m); }
  info(m) { this.tag(35, ":", m); }
  warn(m) { this.tag(33, "?", m); }
  err(m) { this.hadError = true; this.tag(31, "!", m); }

  async _main() {
    for (;;) {
      if (!this.user) await this.sessionBoot();
      const line = await this.readLine();
      await this.runLine(line);
    }
  }

  async readField(prompt, secret) {
    this.emit(`\x1b[0m${prompt} \x1b[36m••>  \x1b[0m`);
    let buf = "";
    for (;;) {
      const c = await this.key(null);
      if (c === 13 || c === 10) { this.emit("\n"); return buf; }
      if ((c === 8 || c === 127) && buf) { buf = buf.slice(0, -1); this.emit("\b \b"); continue; }
      if (c >= 32 && c < 127) { buf += String.fromCharCode(c); this.emit(secret ? "•" : String.fromCharCode(c)); }
    }
  }

  async sessionBoot() {
    this.info("=== Login ===");
    for (;;) {
      const name = await this.readField("Username", false);
      if (!name) { this.warn("Please enter a username."); continue; }
      this.loginTyped.push(name);
      if (!(name in this.users)) { this.warn(`User '${name}' not found.`); continue; }
      if (this.users[name] === null) { this.info(`No password required for '${name}'.`); break; }
      const pw = await this.readField("Password", true);
      if (!pw) { this.warn("Password cannot be blank."); continue; }
      this.loginTyped.push("<password>");
      if (pw === this.users[name]) break;
      this.warn("Incorrect password.  Attempt 1/3.");
      return this.sessionBoot();
    }
    this.user = this.loginTyped.filter((n) => n !== "<password>").pop();
    this.ok(`Welcome, ${this.user}!`);
  }

  prompt() { return `\x1b[36m${this.user}\x1b[90m@\x1b[0mvela:\x1b[34m${this.cwd}\x1b[0m\x1b[36m>\x1b[0m `; }

  async readLine() {
    const p = this.prompt();
    this.emit(p);
    let buf = "";
    for (;;) {
      const c = await this.key(null);
      if (c === 13 || c === 10) { this.emit("\n"); return buf; }
      if (c === 3) { this.emit("^C\n" + p); buf = ""; continue; }
      if ((c === 8 || c === 127)) { if (buf) { buf = buf.slice(0, -1); this.emit("\b \b"); } continue; }
      if (c === 0x1b) { let k; do { k = await this.key(30); } while (k !== null && !(k >= 0x40 && k <= 0x7e && k !== 0x5b)); continue; }
      if (c >= 32 && c < 127) { buf += String.fromCharCode(c); this.emit(String.fromCharCode(c)); }
    }
  }

  static segments(line) {
    const segs = []; let cur = ""; let kind = "first"; let q = null;
    for (let i = 0; i < line.length; i++) {
      const ch = line[i];
      if (q) { cur += ch; if (ch === q) q = null; continue; }
      if ((ch === "'" || ch === '"') && (!cur || cur.endsWith(" "))) { q = ch; cur += ch; continue; }
      const two = line.slice(i, i + 2);
      if (two === "&&" || two === "||" || ch === ";") {
        segs.push([kind, cur.trim()]); cur = "";
        kind = two === "&&" ? "and" : two === "||" ? "or" : "seq";
        if (two === "&&" || two === "||") i++;
        continue;
      }
      cur += ch;
    }
    segs.push([kind, cur.trim()]);
    return segs;
  }

  static args(seg) {
    const out = []; let i = 0;
    while (i < seg.length) {
      while (seg[i] === " ") i++;
      if (i >= seg.length) break;
      let w = "";
      if (seg[i] === "'" || seg[i] === '"') {
        const j = seg.indexOf(seg[i], i + 1);
        if (j < 0) { w = seg.slice(i + 1); i = seg.length; } else { w = seg.slice(i + 1, j); i = j + 1; }
      }
      while (i < seg.length && seg[i] !== " ") w += seg[i++];
      out.push(w);
    }
    return out;
  }

  async runLine(line) {
    let status = 0;
    for (const [kind, seg] of MockDevice.segments(line)) {
      if ((kind === "and" && status) || (kind === "or" && !status) || !seg) continue;
      this.hadError = false;
      status = (await this.command(MockDevice.args(seg))) || 0;
      if (!status && this.hadError) status = 1;
    }
    return status;
  }

  static isSystem(p) { return p === "/os" || p === "/etc" || p.startsWith("/os/") || p.startsWith("/etc/"); }

  needsSudo(p) {
    return this.proto >= 2 && this.admins.has(this.user) && MockDevice.isSystem(p) && !this.elevated;
  }

  async sudo(a) {
    if (a.length < 2) { this.out("Usage: sudo <command> [args...]\n"); return 1; }
    if (a[1] === "-k") { this.grace = false; return 0; }
    if (!this.admins.has(this.user)) {
      this.err(`${this.user} is not an admin. sudo cannot grant what the account does not have.`);
      return 1;
    }
    if (this.proto < 2) return this.command(a.slice(1));
    if (!this.elevated && !this.grace) {
      // Only link mode reaches this mock's shell, and EXEC never asks.
      this.sudoBlocked = true;
      this.err("sudo needs a password and cannot ask for one here; run `sudo -v` first.");
      return 1;
    }
    if (a[1] === "-v") return 0;
    const was = this.elevated;
    this.elevated = true;
    try { return await this.command(a.slice(1)); } finally { this.elevated = was; }
  }

  async command(a) {
    const fs = this.files, dirs = this.dirs, r = (p) => resolve(this.cwd, p);
    if (a[0] === "sudo") return this.sudo(a);
    switch (a[0]) {
      case "echo": this.out(a.slice(1).join(" ") + "\n"); return 0;
      case "whoami": this.out(this.user + "\n"); return 0;
      case "fail": this.err("it failed"); return 0;
      case "cd": { const p = r(a[1] || "/"); if (!dirs.has(p)) { this.err(`No such directory: ${p}`); return 1; } this.cwd = p; return 0; }
      case "cat": { const p = r(a[1] || ""); if (!fs.has(p)) { this.err(`No such file: ${p}`); return 1; } this.out(fs.get(p)); return 0; }
      case "ls": { const p = r(a[1] || "."); for (const e of this.list(p)) this.out(`  ${e.type === "d" ? "DIR " : "FILE"}  ${e.name}\n`); return 0; }
      case "mkdir": {
        const p = r(a[1] || "");
        if (dirs.has(p) || fs.has(p) || !dirs.has(dirname(p))) { this.err(`Could not create ${p}`); return 1; }
        dirs.add(p); return 0;
      }
      case "rm": {
        let rest = a.slice(1); const rec = rest[0] === "-r"; if (rec) rest = rest.slice(1);
        for (const x of rest) {
          const p = r(x);
          if (SYSTEM.includes(p)) this.err(`${p} is a system directory and stays.`);
          else if (fs.has(p)) fs.delete(p);
          else if (dirs.has(p)) {
            if (!rec) this.err(`${p} is a directory; rm -r removes it`);
            else {
              for (const f of [...fs.keys()]) if (f.startsWith(p + "/")) fs.delete(f);
              for (const d of [...dirs]) if (d === p || d.startsWith(p + "/")) dirs.delete(d);
            }
          } else this.err(`No such file: ${p}`);
        }
        return 0;
      }
      case "mv": {
        const x = r(a[1] || ""); let y = r(a[2] || "");
        if (dirs.has(y)) y = y + "/" + basename(x);
        if (fs.has(x)) { fs.set(y, fs.get(x)); fs.delete(x); return 0; }
        if (dirs.has(x) && !SYSTEM.includes(x)) {
          for (const f of [...fs.keys()]) if (f.startsWith(x + "/")) { fs.set(y + f.slice(x.length), fs.get(f)); fs.delete(f); }
          for (const d of [...dirs]) if (d === x || d.startsWith(x + "/")) { dirs.delete(d); dirs.add(y + d.slice(x.length)); }
          return 0;
        }
        this.err(`Could not move ${x}`); return 1;
      }
      case "meminfo": this.info("Memory"); this.out("  Total : 480 KB\n  Free  : 280 KB\n  Largest block : 180 KB\n"); return 0;
      case "df": this.info("Filesystem"); this.out("  Total : 3072 KB\n  Used  : 100 KB  (3%)\n  Free  : 2972 KB\n"); return 0;
      case "ps": this.out("  PID  STATE  NAME\n    1  wait   init\n    2  run    shell\n"); return 0;
      case "logdump": this.out("[0.412] boot ok\n"); return 0;
      case "sleep": {
        const end = Date.now() + 1000 * Number(a[1] || 1);
        while (Date.now() < end) if ((await this.key(10)) === 3) { this.warn("Interrupted."); return 130; }
        return 0;
      }
      case "pkg": {
        if (this.proto >= 2 && (a[1] === "install" || a[1] === "remove") && !this.elevated) {
          this.err(`'pkg ${a[1]}' changes the system. Run it with sudo: sudo ${a.join(" ")}`);
          return 1;
        }
        if (a[1] === "list") { this.info("Installed packages:"); for (const k of Object.keys(this.installed).sort()) this.out(`  ${k.padEnd(14)} ${this.installed[k]}\n`); return 0; }
        if (a[1] === "install" && a[2]) {
          const p = r(a[2]);
          if (!fs.has(p)) { this.err(`No such file: ${p}`); return 1; }
          const head = dec.decode(fs.get(p).slice(0, 64)).split("\n")[0].split(" ");
          if (head[0] !== "RPCAPP") { this.err("Not a valid package: bad magic"); return 1; }
          fs.set(`/os/pkg/${head[1]}.app`, fs.get(p));
          this.installed[head[1]] = head[2];
          this.ok(`[pkg] Installed ${head[1]} ${head[2]}`);
          return 0;
        }
        this.out("Usage: pkg install|list ...\n"); return 1;
      }
      case "reboot":
        this.info("Rebooting system...");
        if (this.onReboot) this.onReboot();
        await new Promise(() => {});   // gone
        return 0;
      case "link":
        if (this.linkSupported) return this.link();
        break;
      default:
    }
    this.err(`'${a[0]}' is not a command or executable file.`);
    return 1;
  }

  list(p) {
    const out = [];
    for (const d of this.dirs) if (d !== "/" && dirname(d) === p) out.push({ type: "d", size: 0, name: basename(d) });
    for (const [f, v] of this.files) if (dirname(f) === p) out.push({ type: "f", size: v.length, name: basename(f) });
    return out.sort((x, y) => (x.name < y.name ? -1 : 1));
  }

  // --- link mode -----------------------------------------------------------

  frame(text) {
    if (this.noise && this.rand() < this.noise) this.emit(`\x1b[35m[:]\x1b[0m [svc] background tick ${Math.floor(this.rand() * 999)}\n`);
    this.emit(concat(concat(new Uint8Array([0x1e]), typeof text === "string" ? enc.encode(text) : text), enc.encode("\n")));
  }

  async request(timeoutMs) {
    const buf = [];
    let last = Date.now();
    for (;;) {
      const c = await this.key(10);
      if (c === null) {
        if (timeoutMs && Date.now() - last >= timeoutMs) return { r: "timeout" };
        continue;
      }
      last = Date.now();
      if (c === 3) return { r: "intr" };
      if (c === 13) continue;
      if (c === 10) return buf.length >= 1100 ? { r: "long" } : { r: "ok", line: dec.decode(new Uint8Array(buf)) };
      buf.push(c);
    }
  }

  hello() {
    const cwd = this.cwd.replace(/%/g, "%25").replace(/ /g, "%20");
    this.frame(`HELLO proto=${this.proto} os=v2.01-beta build=554 board=${this.board} arch=${/^pico(_w)?$/.test(this.board) ? "armv6m" : "armv8m"} user=${this.user} cwd=${cwd} chunk=768 exec_max=32768`);
  }

  async link() {
    this.emit("Link mode, for host tools (LINK.md). Ctrl+C returns to the prompt.\n");
    this.inLink = true;
    this.hello();
    for (;;) {
      const q = await this.request(0);
      if (q.r === "intr") break;
      if (q.r === "long") { this.frame("ERR badreq line too long"); continue; }
      if (!q.line) continue;
      this.requests.push(q.line);
      const sp = q.line.indexOf(" ");
      const verb = sp < 0 ? q.line : q.line.slice(0, sp);
      const rest = sp < 0 ? "" : q.line.slice(sp + 1);
      if (verb === "PING") this.frame("OK");
      else if (verb === "HELLO") { this.hello(); this.frame("OK"); }
      else if (verb === "EXEC") await this.lExec(rest);
      else if (verb === "PUT") await this.lPut(rest);
      else if (verb === "GET") await this.lGet(rest);
      else if (verb === "LS") this.lLs(rest);
      else if (verb === "STAT") this.lStat(rest);
      else if (verb === "AUTH" && this.proto >= 2) this.lAuth(rest);
      else if (verb === "BYE") { this.byes++; this.frame("OK"); break; }
      else this.frame("ERR badreq unknown verb " + verb);
    }
    this.inLink = false;
    this.frame("BYE");          // the firmware says so however link mode ends
    this.emit("Left link mode.\n");
    return 0;
  }

  async lExec(line) {
    this.capture = [];
    this.sudoBlocked = false;
    let rc;
    try { rc = await this.runLine(line); } catch (e) { rc = 1; }
    const out = this.capture.reduce(concat, new Uint8Array(0));
    this.capture = null;
    for (let i = 0; i < out.length; i += 768) this.frame("OUT " + b64(out.subarray(i, i + 768)));
    if (this.sudoBlocked) {
      this.sudoBlocked = false;
      return this.frame("ERR sudo sudo needs a password: send AUTH, then the request again");
    }
    this.frame(`OK rc=${rc ? 1 : 0} truncated=0`);
  }

  lAuth(rest) {
    let pw = null;
    try { pw = rest ? dec.decode(Uint8Array.from(atob(rest), (c) => c.charCodeAt(0))) : null; } catch (e) { pw = null; }
    if (pw === null) return this.frame("ERR badreq AUTH <password in base64>");
    this.auths.push(pw);
    if (!this.admins.has(this.user)) return this.frame(`ERR perm ${this.user} is not an admin`);
    if (pw !== this.users[this.user]) return this.frame("ERR auth wrong password");
    this.grace = true;
    this.frame("OK grace=300");
  }

  async lPut(rest) {
    const m = /^(\d+) ([0-9a-fA-F]{64}) (.+)$/.exec(rest);
    if (!m) return this.frame("ERR badreq PUT <size> <sha256> <path>");
    const size = Number(m[1]); const p = resolve(this.cwd, m[3]);
    if (this.needsSudo(p) && !this.grace) return this.frame(`ERR sudo ${p} is part of the system: send AUTH first`);
    if (this.dirs.has(p)) return this.frame(`ERR nofile ${p} is a directory`);
    if (!this.dirs.has(dirname(p))) return this.frame(`ERR io could not open ${p} for writing`);
    this.frame("READY chunk=768");
    let got = new Uint8Array(0);
    while (got.length < size) {
      const q = await this.request(10000);
      if (q.r === "intr" || (q.r === "ok" && q.line === "CANCEL")) { this.files.delete(p); return this.frame("ERR cancelled cancelled by the host"); }
      if (q.r !== "ok" || !q.line.startsWith("D ")) { this.files.delete(p); return this.frame("ERR badreq expected a D line"); }
      const s = atob(q.line.slice(2));
      const d = new Uint8Array(s.length);
      for (let i = 0; i < s.length; i++) d[i] = s.charCodeAt(i);
      got = concat(got, d);
      this.frame(`ACK ${got.length}`);
    }
    const hex = await sha256hex(got);
    if (hex !== m[2].toLowerCase()) { this.files.delete(p); return this.frame(`ERR digest received ${hex}; nothing was kept`); }
    this.files.set(p, got);
    this.frame("OK sha=" + hex);
  }

  async lGet(rest) {
    const p = resolve(this.cwd, rest);
    if (this.dirs.has(p)) return this.frame(`ERR nofile ${p} is a directory`);
    if (!this.files.has(p)) return this.frame(`ERR nofile there is no ${p}`);
    const data = this.files.get(p);
    this.frame(`SIZE ${data.length}`);
    for (let off = 0; off < data.length; off += 768) {
      let piece = data.slice(off, off + 768);
      if (this.corruptGet && off === 0) { piece = piece.slice(); piece[0] ^= 1; }
      this.frame("D " + b64(piece));
      const q = await this.request(10000);
      if (q.r === "intr" || (q.r === "ok" && q.line === "CANCEL")) return this.frame("ERR cancelled cancelled by the host");
      if (q.r !== "ok" || !q.line.startsWith("A")) return this.frame("ERR badreq expected A");
    }
    this.frame("OK sha=" + (await sha256hex(data)));
  }

  lLs(rest) {
    const p = resolve(this.cwd, rest || ".");
    if (!this.dirs.has(p)) return this.frame(this.files.has(p) ? `ERR nofile ${p} is not a directory` : `ERR nofile there is no ${p}`);
    const ents = this.list(p);
    for (const e of ents) this.frame(`E ${e.type} ${e.size} ${e.name}`);
    this.frame(`OK n=${ents.length}`);
  }

  lStat(rest) {
    const p = resolve(this.cwd, rest);
    if (this.dirs.has(p)) return this.frame("OK type=d size=0");
    if (this.files.has(p)) return this.frame(`OK type=f size=${this.files.get(p).length}`);
    this.frame(`ERR nofile there is no ${p}`);
  }
}
