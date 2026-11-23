// A pretend RPCortex v2 device, for the tests and for the page's demo mode
// (index.html?demo). It is the JavaScript twin of tools/rpc/tests/mockdev.py,
// smaller: the prompt and its echo, the login prompt, firmware without link
// mode, and link mode as os/shell/link.cpp implements it.

import { versionCmp } from "./version.js";

const enc = new TextEncoder();
const dec = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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

// A pretend firmware image: what `update from-file` reads its version from.
// The real one is checked by its vector table; this one by its first line.
export function makeImage(board, os, build, size = 4096) {
  const head = enc.encode(`RPCIMG ${board} ${os} ${build}\n`);
  const out = new Uint8Array(Math.max(size, head.length));
  out.set(head, 0);
  for (let i = head.length; i < out.length; i++) out[i] = (i * 13) & 0xff;
  return out;
}

const BOARD_RADIO = { pico2_w: true, pico_w: true, pico2: false, pico: false };

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
                proto = 2, os = "v2.01-beta", build = "554", network = true, manifest = null,
                capacity = 16 << 20, updateDelay = 30, fromFileYes = true } = {}) {
    this.linkSupported = link;
    // Firmware with `update from-file --yes` (false: one from before it).
    this.fromFileYes = fromFileYes;
    // What HELLO and `update` say this firmware is.
    this.os = os;
    this.build = String(build);
    // The update: `manifest` is latest.json as the device would fetch it;
    // `network` is whether Wi-Fi is up (a board without a radio never has it).
    this.radio = BOARD_RADIO[board] !== false;
    this.network = network && this.radio;
    this.manifest = manifest;
    this.updateDelay = updateDelay;
    this.updateFails = null;        // a message: the download "fails" with it
    this.updates = [];              // the update commands that reached the device
    this.gone = false;              // restarting: the port is closed
    this.restarts = 0;
    // The filesystem's size. PUT refuses what would take it past 98%, without
    // counting the file it replaces, as link.cpp does.
    this.capacity = capacity;
    this.linkAuthed = false;
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
    put("/os/users.cfg", "root:salted:hash\n");
    put("/os/pkg/greet.app", makeApp("greet"));
    put("/etc/startup.cfg", "# startup\n");
    put("/home/root/notes.txt", "hello from the device\n");
    put("/usb/readme.txt", "drop files here\n");
  }

  rand() { this.seed = (this.seed * 1103515245 + 12345) & 0x7fffffff; return this.seed / 0x7fffffff; }

  // the host's end: write(bytes), read(ms), discard(). After a restart the
  // port is gone, as a real one is: reading and writing throw, the way
  // serial.js does once the USB device has dropped.
  stream() {
    const q = { to: this.toHost, from: this.fromHost };
    const lost = () => this.gone || q.to !== this.toHost;
    return {
      write: async (b) => { if (lost()) throw new Error("the port is closed"); q.from.push(b); },
      read: async (ms) => {
        if (lost()) throw new Error("the port closed");
        try { return await q.to.take(1 << 16, ms); } catch (e) {
          if (lost()) throw new Error("the port closed");
          return new Uint8Array(0);
        }
      },
      discard: () => q.to.clear(),
      get closed() { return lost(); },
    };
  }

  start() { this._main().catch(() => {}); return this; }
  stop() { this.toHost.close(); this.fromHost.close(); }

  // The board restarting: the port drops and everything in memory goes. What
  // runs from here never returns, like the code after a firmware write.
  async restart(to) {
    this.gone = true;
    this.restarts++;
    if (to) { this.os = to.os; this.build = String(to.build); }
    this.toHost.close();
    this.fromHost.close();
    if (this.onReboot) this.onReboot();
    await new Promise(() => {});
  }

  // ...and coming back: a fresh port, the shell at its prompt (signed in, as
  // autonomy does), no sudo grace, nothing in link mode.
  revive() {
    this.toHost = new ByteQueue();
    this.fromHost = new ByteQueue();
    this.gone = false;
    this.inLink = false;
    this.grace = false;
    this.linkAuthed = false;
    this.elevated = false;
    this.capture = null;
    this.cwd = "/";
    for (const p of [...this.files.keys()]) if (p.startsWith("/tmp/")) this.files.delete(p);
    this.start();
    return this;
  }

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

  // RpcAppHeader (rpc_app.h): magic 'RPCA', api major and minor, name[24],
  // version[12]. Null when the bytes hold none.
  static appHeader(data) {
    const cstr = (o, n) => { let s = ""; for (let i = o; i < o + n && data[i]; i++) s += String.fromCharCode(data[i]); return s; };
    for (let i = 0; i + 48 <= data.length; i++) {
      if (data[i] !== 0x41 || data[i + 1] !== 0x43 || data[i + 2] !== 0x50 || data[i + 3] !== 0x52) continue;
      if (data[i + 4] !== 1 || data[i + 5] !== 0) continue;          // api_major 1
      const name = cstr(i + 8, 24), version = cstr(i + 32, 12);
      if (/^[A-Za-z0-9_.-]+$/.test(name) && /^[A-Za-z0-9_.-]+$/.test(version)) return { name, version };
    }
    return null;
  }

  static isSystem(p) { return p === "/os" || p === "/etc" || p.startsWith("/os/") || p.startsWith("/etc/"); }

  needsSudo(p) {
    return this.proto >= 2 && this.admins.has(this.user) && MockDevice.isSystem(p) && !this.elevated;
  }

  readNeedsSudo(p) {
    return this.proto >= 2 && this.admins.has(this.user) && !this.elevated && !this.grace &&
      (["/os/users.cfg", "/os/users.bak", "/os/device.key"].includes(p) || p === "/os/ssh" || p.startsWith("/os/ssh/"));
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
      if (this.capture) {
        // EXEC never asks (TaskShell::noprompt).
        this.sudoBlocked = true;
        this.err("sudo needs a password and cannot ask for one here; run `sudo -v` first.");
        return 1;
      }
      // At the console's prompt sudo asks, three tries, as core/sudo.cpp does.
      let good = false;
      for (let i = 0; i < 3 && !good; i++) {
        const pw = await this.readField(`Password for ${this.user}, to run '${a.slice(1, 3).join(" ")}'`, true);
        this.auths.push(pw);
        good = pw === this.users[this.user];
        if (!good) this.warn("Sorry, try again.");
      }
      if (!good) { this.err("Three wrong passwords. Nothing was run."); return 1; }
      this.grace = true;
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
          else if (this.needsSudo(p)) this.err(`${p} is part of the system. Run it with sudo: sudo rm ${x}`);
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
      case "df": {
        const used = this.used(), cap = this.capacity;
        this.info("Filesystem");
        this.out(`  Total : ${cap >> 10} KB\n  Used  : ${used >> 10} KB  (${Math.round((100 * used) / cap)}%)\n  Free  : ${(cap - used) >> 10} KB\n`);
        return 0;
      }
      case "update": return this.update(a);
      case "_pkgs":
        this.out("PKGS_BEGIN\n" + `OS:${this.os}:${this.board}\n`);
        for (const k of Object.keys(this.installed).sort()) this.out(`PKG:${k}:${this.installed[k]}\n`);
        this.out("PKGS_END\n");
        return 0;
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
          let head = dec.decode(fs.get(p).slice(0, 64)).split("\n")[0].split(" ");
          if (head[0] !== "RPCAPP") {
            // A real package (the demo installs them): RpcAppHeader, found
            // by its magic, with the name and version inside it.
            const real = MockDevice.appHeader(fs.get(p));
            if (real) head = ["RPCAPP", real.name, real.version];
          }
          if (head[0] !== "RPCAPP") { this.err("Not a valid package: bad magic"); return 1; }
          fs.set(`/os/pkg/${head[1]}.app`, fs.get(p));
          this.installed[head[1]] = head[2];
          this.ok(`[pkg] Installed ${head[1]} ${head[2]}`);
          return 0;
        }
        if (a[1] === "remove" && a[2]) {
          const name = Object.keys(this.installed).find((k) => k.toLowerCase() === a[2].toLowerCase());
          if (!name) { this.err(`Not installed: ${a[2]}`); return 1; }
          delete this.installed[name];
          fs.delete(`/os/pkg/${name}.app`);
          this.ok(`[pkg] Removed ${name}`);
          return 0;
        }
        this.out("Usage: pkg install|list|remove ...\n"); return 1;
      }
      case "reboot":
        this.info("Rebooting system...");
        await this.restart();
        return 0;
      case "link":
        if (this.linkSupported) return this.link();
        break;
      default:
    }
    this.err(`'${a[0]}' is not a command or executable file.`);
    return 1;
  }

  used() {
    let n = 0;
    for (const v of this.files.values()) n += v.length;
    return n;
  }

  // The yes/no question session_confirm asks. It reads keys until Enter, and
  // under EXEC nobody sends any: the command waits for ever, as on a board.
  async confirm(q) { return (await this.readField(`${q} (yes/no)`, false)) === "yes"; }

  // update: check, install/online [--force] [--yes], from-file, rollback
  // --forget, after update.cpp and update_net.cpp. The comparison is the
  // device's (repo_version_cmp against version-and-build).
  async update(a) {
    const sub = a[1] || "check";
    const buildver = () => {
      let num = this.os.replace(/^v/, "");
      if (num.includes("-")) num = num.slice(0, num.indexOf("-"));
      return num.endsWith("." + this.build) ? num : `${num}.${this.build}`;
    };
    const entry = () => {
      const list = (this.manifest && this.manifest.packages) || [];
      return list.find((p) => p.name.toLowerCase() === this.board) || null;
    };
    const changes = sub === "install" || sub === "online" || sub === "from-file" ||
      (sub === "rollback" && a[2] && a[2] !== "status");
    if (changes && this.proto >= 2 && !this.elevated) {
      this.err(`'update ${sub}' changes the system. Run it with sudo: sudo ${a.join(" ")}`);
      return 1;
    }
    if (sub === "check" || sub === "install" || sub === "online") {
      if (!this.radio) {
        this.err("This board has no wireless hardware.");
        this.out("  'update from-file <image.bin>' applies an image copied onto the device.\n");
        return 1;
      }
      let force = false, yes = false;
      for (const o of a.slice(2)) {
        if (o === "--force" || o === "-f") force = true;
        else if (o === "--yes" || o === "-y") yes = true;
        else { this.err(`Unknown option: ${o}`); return 1; }
      }
      if (sub !== "check") this.updates.push({ sub, force, yes });
      this.info("Checking for an update...");
      if (!this.network) { this.err("No network. Connect first."); return 1; }
      const e = entry();
      if (!e) { this.err(`No release listed for this board (${this.board}).`); return 1; }
      const cmp = versionCmp(e.ver, buildver());
      if (sub === "check") {
        this.out(`  Installed  ${this.os}\n  Available  ${e.ver}\n`);
        if (cmp <= 0) this.ok("Already up to date.");
        else this.info("An update is available. 'sudo update install' to fetch and apply it.");
        return 0;
      }
      if (!force && cmp <= 0) {
        this.ok(`Already running ${this.os}. 'sudo update install --force' to reinstall.`);
        return 0;
      }
      this.info(`Downloading ${e.ver} (${(e.size || 0) >> 10} KB)...`);
      await sleep(this.updateDelay);
      if (this.updateFails) { this.err(this.updateFails); return 1; }
      this.ok("Checksum verified.");
      if (!yes && !(await this.confirm("  Continue?"))) { this.info("Cancelled. The download is kept."); return 0; }
      const os = (/v\d\S*/.exec(e.desc || "") || ["v" + e.ver])[0];
      this.info("Writing firmware...");
      return this.restart({ os, build: String(e.ver).split(".").pop() });
    }
    if (sub === "from-file") {
      const yes = a.slice(2).some((x) => x === "--yes" || x === "-y");
      const rest = a.slice(2).filter((x) => x !== "--yes" && x !== "-y");
      if (yes && !this.fromFileYes) { this.err("Unknown option: --yes"); return 1; }
      if (!rest[0]) { this.out("Usage: update from-file <image.bin>\n"); return 1; }
      const p = resolve(this.cwd, rest[0]);
      this.updates.push({ sub, path: p });
      this.warn(`Writing ${rest[0]} over the firmware, unverified.`);
      this.out("  A local file has no checksum to check it against.\n");
      if (!yes && !(await this.confirm("  Continue?"))) return 0;
      const data = this.files.get(p);
      if (!data) { this.err(`No image at ${p}.`); return 1; }
      let head = dec.decode(data.slice(0, 80)).split("\n")[0].split(" ");
      if (head[0] !== "RPCIMG") {
        // A real image (the demo sends one): the vector table where this
        // board's chip keeps it, and the version string inside.
        const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const at = /^pico(_w)?$/.test(this.board) ? 0x100 : 0;
        const sp = data.length > at + 8 ? v.getUint32(at, true) : 0, entry = data.length > at + 8 ? v.getUint32(at + 4, true) : 0;
        let text = "";
        for (let i = 0; i < data.length; i += 0x8000) text += String.fromCharCode.apply(null, data.subarray(i, i + 0x8000));
        const ver = /v2\.\d+(?:\.\d+)?(?:-[a-z0-9]+)?/.exec(text);
        if (sp >= 0x20000000 && sp < 0x20090000 && entry >= 0x10000000 && entry < 0x10200000 && ver) {
          head = ["RPCIMG", this.board, ver[0], (/^v\d+\.\d+\.(\d+)/.exec(ver[0]) || [0, "0"])[1]];
        }
      }
      if (head[0] !== "RPCIMG" || head[1] !== this.board) {
        this.err("The staged image does not look like firmware for this board.");
        return 1;
      }
      this.info(`Staging ${data.length >> 10} KB...`);
      // Only the file update itself downloaded is removed (update.cpp).
      if (p === "/os/update.bin") this.files.delete(p);
      this.ok("Staged and verified.");
      this.info("Writing firmware...");
      return this.restart({ os: head[2], build: head[3] });
    }
    if (sub === "rollback" && a[2] === "--forget") {
      const had = this.files.get("/os/rollback.img");
      this.files.delete("/os/rollback.img");
      this.ok(had ? `Removed the saved image. ${had.length >> 10} KB freed.` : "There was nothing saved. Cleaned up anyway.");
      return 0;
    }
    this.out("Usage:\n  update check\n  update install\n  update from-file <path>\n  update rollback\n");
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
    this.frame(`HELLO proto=${this.proto} os=${this.os} build=${this.build} board=${this.board} arch=${/^pico(_w)?$/.test(this.board) ? "armv6m" : "armv8m"} user=${this.user} cwd=${cwd} chunk=768 exec_max=32768`);
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
    // A grace AUTH started ends with link mode (link.cpp), so sudo typed at
    // the prompt afterwards asks again.
    if (this.linkAuthed) { this.grace = false; this.linkAuthed = false; }
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
    this.linkAuthed = true;
    this.frame("OK grace=300");
  }

  async lPut(rest) {
    const m = /^(\d+) ([0-9a-fA-F]{64}) (.+)$/.exec(rest);
    if (!m) return this.frame("ERR badreq PUT <size> <sha256> <path>");
    const size = Number(m[1]); const p = resolve(this.cwd, m[3]);
    if (this.needsSudo(p) && !this.grace) return this.frame(`ERR sudo ${p} is part of the system: send AUTH first`);
    if (this.dirs.has(p)) return this.frame(`ERR nofile ${p} is a directory`);
    if (!this.dirs.has(dirname(p))) return this.frame(`ERR io could not open ${p} for writing`);
    // storage_would_fit: the size against what is free, nothing past 98%
    // full, and the file being replaced still counted as used.
    const free = Math.floor(this.capacity * 0.98) - this.used();
    if (size > free) return this.frame(`ERR nospace ${Math.max(0, this.capacity - this.used())} bytes free`);
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
    if (this.readNeedsSudo(p)) return this.frame(`ERR sudo ${p} needs sudo: send AUTH first`);
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
