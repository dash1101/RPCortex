// The page: one serial port, a terminal on it, and link mode for everything
// else. Switching panes switches the device between the two.

import {
  enterLink, joinPath, parentPath, quote, LinkError, RpcError,
} from "./link.js";
import { linkArch, packagesPane } from "./pkgs.js";
import { SerialLine, RPI_VID, resetToBootsel, webSerialSupported } from "./serial.js";
import { createTerminal } from "./terminal.js";
import { firmwarePane } from "./updates.js";

const $ = (s) => document.querySelector(s);
const enc = new TextEncoder();
const LINK_PANES = new Set(["files", "run", "packages", "device"]);
// Panes with an error line of their own (`<pane>-error`), where a failure is
// shown beside what it was about rather than in a toast.
const ERROR_PANES = new Set(["files", "run", "packages", "firmware", "device"]);
const SYSTEM_DIRS = new Set(["/os", "/os/pkg", "/etc", "/home", "/tmp", "/usb"]);
const DEMO = new URLSearchParams(location.search).has("demo");

const S = {
  serial: null,       // navigator.serial, or the demo's stand-in
  port: null,         // the SerialPort last opened
  line: null,         // SerialLine while connected
  mode: "off",        // off | terminal | switching | link | legacy
  link: null,
  term: null,
  pane: "connect",
  cwd: "/",
  runAbort: null,
  fw: null,           // the Firmware pane (updates.js)
  pk: null,           // the Packages pane (pkgs.js)
};

// --- small helpers ----------------------------------------------------------------

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

function human(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}

function toast(text, bad = false) {
  const t = el("div", { class: "toast" + (bad ? " bad" : "") }, text);
  $("#toasts").append(t);
  setTimeout(() => t.remove(), bad ? 9000 : 4500);
}

function explain(e) {
  if (e instanceof LinkError) return e.detail ? `${e.detail} (${e.code})` : e.code;
  return (e && e.message) || String(e);
}

// A failure goes on the error line of the pane it happened in, which stays
// until it is dismissed or the pane is opened again; elsewhere, a toast.
function showError(pane, e) {
  const box = pane && ERROR_PANES.has(pane) ? $(`#${pane}-error`) : null;
  if (!box) { toast(explain(e), true); return; }
  box.textContent = "";
  box.append(el("span", {}, explain(e)), el("button", {
    class: "dismiss", "aria-label": "Dismiss", title: "Dismiss", onclick: () => { box.hidden = true; },
  }, "\u00d7"));
  box.hidden = false;
  box.scrollIntoView({ block: "nearest" });
}

function clearError(pane) { const box = $(`#${pane}-error`); if (box) box.hidden = true; }

function fail(e) { console.error(e); showError(S.pane, e); }

// A question in a dialog. Resolves to the typed text (or true), or null.
function ask({ title, body = "", input = null, ok = "OK", danger = false, secret = false }) {
  const d = $("#ask");
  $("#ask-title").textContent = title;
  $("#ask-body").textContent = body;
  const inp = $("#ask-input");
  inp.hidden = input === null;
  inp.value = input || "";
  // A password field for a password: masked, and offered to a password manager.
  inp.type = secret ? "password" : "text";
  inp.autocomplete = secret ? "current-password" : "off";
  const okBtn = $("#ask-ok");
  okBtn.textContent = ok;
  okBtn.className = danger ? "danger" : "primary";
  return new Promise((resolve) => {
    // Enter in the field means OK. Left to the form, it would press the first
    // button, which is Cancel.
    const enter = (e) => { if (e.key === "Enter" && !e.isComposing) { e.preventDefault(); d.close("ok"); } };
    inp.addEventListener("keydown", enter);
    d.addEventListener("close", () => {
      inp.removeEventListener("keydown", enter);
      const value = secret ? inp.value : inp.value.trim();
      // A password is handed over once and not left in the page.
      if (secret) inp.value = "";
      if (d.returnValue !== "ok") resolve(null);
      else resolve(input === null ? true : value || null);
    }, { once: true });
    d.returnValue = "";
    d.showModal();
    if (input !== null) { inp.focus(); inp.select(); } else okBtn.focus();
  });
}

// --- mode and the badge -------------------------------------------------------

const MODE_TEXT = {
  off: "Not connected", terminal: "Terminal", switching: "Switching", link: "Link mode",
  legacy: "Firmware without link mode",
};

function setMode(m) {
  S.mode = m;
  document.body.dataset.mode = m;
  $("#conn-mode").textContent = MODE_TEXT[m];
  const on = m !== "off";
  $("#btn-disconnect").disabled = !on;
  $("#btn-bootsel").disabled = !on;
  $("#btn-ctrlc").disabled = !on;
  renderPrompt();
}

function renderPrompt() {
  const p = $("#conn-prompt");
  p.textContent = "";
  if (S.mode === "off") { p.textContent = "no device"; return; }
  const h = S.link ? S.link.hello : null;
  if (!h || !h.user) { p.textContent = S.line ? portName() : ""; return; }
  p.append(el("span", { class: "u" }, h.user), el("span", { class: "h" }, "@" + (h.board || "device")),
    ":", el("span", { class: "p" }, h.cwd || "/"));
  p.title = `${h.os || ""} build ${h.build || "?"}`;
}

function portName() {
  const i = S.line ? S.line.info() : {};
  return i.usbVendorId === RPI_VID ? (i.usbProductId === 0x0011 ? "RPCortex" : "Pico") : "serial port";
}

const pending = [];
function termWrite(bytes) {
  if (S.term) S.term.write(bytes);
  else if (pending.length < 256) pending.push(bytes);
}

function noise(line) {
  const m = $("#messages");
  if (m.dataset.used !== "1") { m.textContent = ""; m.dataset.used = "1"; }
  m.textContent += line + "\n";
  m.scrollTop = m.scrollHeight;
}

// Switches run one after another, never two at once.
let switching = Promise.resolve();
function switchTo(target) {
  const run = switching.then(() => doSwitch(target));
  switching = run.catch(() => {});
  return run;
}

async function doSwitch(target) {
  if (!S.line) throw new RpcError("Not connected. Choose a device on the Connect tab.");
  if (target === "link") {
    if (S.mode === "link" && S.link && !S.link.dropped) return S.link;
    if (S.mode === "legacy") {
      throw new RpcError("This firmware is older than link mode. The Terminal works; Files, Run, Packages and Device need newer firmware.");
    }
    setMode("switching");
    S.line.setSink(null);
    S.line.discard();
    try {
      const user = $("#login-user").value.trim() || undefined;
      const r = await enterLink(S.line, {
        user, onNoise: noise,
        password: async () => $("#login-pass").value || (await ask({ title: `Password for ${user}`, input: "", ok: "Log in", secret: true })),
      });
      if (r.mode === "legacy") {
        S.line.setSink(termWrite);
        setMode("legacy");
        throw new RpcError("This firmware is older than link mode. The Terminal works; Files, Run, Packages and Device need newer firmware.");
      }
      S.link = r.link;
      // sudo (firmware with link protocol 2): installing a package or changing
      // /os or /etc asks for the account's password once, and again if wrong.
      S.link.sudoPassword = (who, wrong) => ask({
        title: wrong ? `Wrong password. Password for ${who}` : `Password for ${who}`,
        body: "This needs sudo on the device. It counts for a few minutes." +
              (DEMO ? " (The demo device's password is demo.)" : ""),
        input: "", ok: "Continue", secret: true,
      });
      S.cwd = S.link.hello.cwd || "/";
      setMode("link");
      renderFacts();
      if (S.pk) S.pk.render();
      if (S.fw) S.fw.refresh();
      return S.link;
    } catch (e) {
      if (S.line && S.mode === "switching") { S.line.setSink(termWrite); setMode("terminal"); }
      throw e;
    }
  }
  // the terminal
  if (S.mode === "link" && S.link) {
    setMode("switching");
    await S.link.bye();
    // What came in behind the final frame ("Left link mode." and the prompt)
    // is the terminal's.
    const rest = S.link.r.buf;
    S.link = null;
    if (rest.length) termWrite(rest);
  }
  if (S.line) {
    S.line.setSink(termWrite);
    if (S.mode !== "legacy") setMode("terminal");
  }
  return null;
}

const needLink = () => switchTo("link");

// --- connecting ------------------------------------------------------------------

// What the browser says when it cannot open a port, in words that say what
// to do about it. DEVICE-UNCONFIRMED: NetworkError for a port another program holds is
// Chrome's documented behaviour, not yet seen through this page.
function openError(e) {
  const n = e && e.name;
  if (n === "NetworkError") return new RpcError("Another program has this port open: a terminal, Thonny, rpc term, or this page in another tab. Close it there, then choose the device again.");
  if (n === "InvalidStateError") return new RpcError("That port is already open in this page. Disconnect first.");
  if (n === "SecurityError") return new RpcError("The browser did not allow this page to open the port.");
  return e;
}

async function openPort(port) {
  const line = new SerialLine(port);
  line.onLost = lost;
  try { await line.open(); } catch (e) { throw openError(e); }
  S.port = port;
  S.line = line;
  line.setSink(termWrite);
  setMode("terminal");
  $("#btn-reconnect").hidden = true;
  $("#connect-status").textContent = `Connected to ${portName()}.`;
  if (LINK_PANES.has(S.pane)) await showPane(S.pane);
}

async function choosePort() {
  try {
    const filters = $("#show-all").checked ? [] : [{ usbVendorId: RPI_VID }];
    const port = await S.serial.requestPort({ filters });
    if (S.line) await disconnect();
    await openPort(port);
  } catch (e) {
    if (e && e.name === "NotFoundError") return;           // the picker was closed
    $("#connect-status").textContent = explain(openError(e));
    fail(openError(e));
  }
}

async function disconnect() {
  const line = S.line;
  if (!line) return;
  // A command still running is stopped first, so the device is not left
  // waiting in the middle of it.
  if (S.runAbort) S.runAbort.abort();
  try { if (S.mode === "link" && S.link) await S.link.bye(); } catch (e) { /* closing anyway */ }
  S.line = null;
  S.link = null;
  await line.close();
  setMode("off");
  $("#connect-status").textContent = "Disconnected. The device is back at its own prompt.";
  if (S.fw) S.fw.refresh();
  if (S.pk) S.pk.render();
}

function lost() {
  S.line = null;
  S.link = null;
  setMode("off");
  $("#btn-reconnect").hidden = false;
  if (S.fw) S.fw.refresh();
  if (S.pk) S.pk.render();
  // A firmware update drops the port on purpose; the Firmware tab says so.
  if (S.fw && S.fw.expectingRestart()) {
    $("#connect-status").textContent = "The device is restarting with its new firmware.";
    return;
  }
  $("#connect-status").textContent = "The device disconnected: it restarted, or the cable came out.";
  toast("The device disconnected. When it is back, choose Reconnect on the Connect tab.");
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function samePort(p) {
  const want = S.port && S.port.getInfo ? S.port.getInfo() : {};
  const i = p.getInfo ? p.getInfo() : {};
  return i.usbVendorId === want.usbVendorId && i.usbProductId === want.usbProductId;
}

// After a restart: wait for the board to come back on USB and open it again.
// The port can be listed before it can be opened, so opening is retried.
// DEVICE-UNCONFIRMED: that Chrome lists the re-enumerated board in getPorts()
// with the same VID/PID after an update; shown with the demo port only.
async function waitForDevice(ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (S.line) return true;
    if (S.port && S.serial) {
      let ports = [];
      try { ports = await S.serial.getPorts(); } catch (e) { ports = []; }
      const port = ports.find(samePort);
      if (port) {
        try { await openPort(port); return true; } catch (e) { /* still enumerating */ }
      }
    }
    await sleep(1500);
  }
  return !!S.line;
}

// The raw console, for a step that has to be typed at the prompt: link mode
// ends, and what the device prints is read here and passed to the Terminal.
async function consoleFor() {
  await switchTo("terminal");
  if (!S.line) throw new RpcError("Not connected.");
  const line = S.line;
  setMode("switching");          // the Terminal's keys stay out of it meanwhile
  line.setSink(null);
  return {
    stream: line,
    echo: termWrite,
    release: () => { if (S.line === line) { line.setSink(termWrite); setMode("terminal"); } },
  };
}

function saveFile(data, name) {
  const url = URL.createObjectURL(new Blob([data]));
  const a = el("a", { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

async function bootselNow() {
  const port = S.port;
  if (!port) return;
  await disconnect();
  await resetToBootsel(port);
  $("#connect-status").textContent = "The board is restarting into BOOTSEL. Look for a drive named RP2350 or RPI-RP2.";
}

async function reconnect() {
  try {
    const ports = await S.serial.getPorts();
    const want = S.port && S.port.getInfo ? S.port.getInfo() : {};
    const port = ports.find((p) => {
      const i = p.getInfo ? p.getInfo() : {};
      return i.usbVendorId === want.usbVendorId && i.usbProductId === want.usbProductId;
    }) || S.port;
    await openPort(port);
  } catch (e) {
    const why = e instanceof RpcError ? e : new RpcError(`Could not reopen the port (${explain(e)}). Choose the device again.`);
    $("#connect-status").textContent = explain(why);
    fail(why);
  }
}

async function bootsel() {
  const yes = await ask({
    title: "Reset to BOOTSEL?",
    body: "The board restarts into its bootloader and this connection closes. A drive appears; copy a .uf2 onto it, or unplug and replug the board to go back. Files on the device are not touched.",
    ok: "Reset to BOOTSEL", danger: true,
  });
  if (!yes) return;
  await bootselNow();
}

// --- panes ------------------------------------------------------------------------

async function showPane(name) {
  S.pane = name;
  for (const b of document.querySelectorAll(".tabs button")) {
    const on = b.dataset.pane === name;
    b.setAttribute("aria-selected", String(on));
    b.tabIndex = on ? 0 : -1;
  }
  for (const p of document.querySelectorAll(".pane")) p.hidden = p.id !== "pane-" + name;
  clearError(name);
  try {
    // These two show the release and the index with no device at all.
    if (name === "firmware" && !S.line) { await S.fw.show(); return; }
    if (name === "packages" && !S.line) { await S.pk.show(); return; }
    if (!S.line) return;
    if (name === "terminal") {
      await switchTo("terminal");
      if (S.term) { S.term.fit(); S.term.focus(); }
    } else if (name === "firmware") {
      S.fw.refresh();
      await needLink().catch((e) => showError("firmware", e));
      await S.fw.show();
    } else if (LINK_PANES.has(name)) {
      await needLink();
      if (name === "files") await loadDir(S.cwd);
      if (name === "packages") await S.pk.show();
      if (name === "device") renderFacts();
      if (name === "run") $("#run-input").focus();
    }
  } catch (e) {
    showError(name, e);
  }
}

// Arrow keys, Home and End move between the tabs (the ARIA tabs pattern);
// Tab moves on into the pane.
function tabKeys(e) {
  const tabs = [...document.querySelectorAll(".tabs button")];
  const i = tabs.indexOf(document.activeElement);
  if (i < 0) return;
  let j = null;
  if (e.key === "ArrowDown" || e.key === "ArrowRight") j = (i + 1) % tabs.length;
  else if (e.key === "ArrowUp" || e.key === "ArrowLeft") j = (i - 1 + tabs.length) % tabs.length;
  else if (e.key === "Home") j = 0;
  else if (e.key === "End") j = tabs.length - 1;
  if (j === null) return;
  e.preventDefault();
  tabs[j].focus();
  showPane(tabs[j].dataset.pane);
}

// --- colours ----------------------------------------------------------------------

const THEME_KEY = "rpc-web-theme";
const THEMES = ["auto", "light", "dark"];
const THEME_TEXT = { auto: "Auto", light: "Light", dark: "Dark" };

function setTheme(t) {
  if (!THEMES.includes(t)) t = "auto";
  if (t === "auto") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = t;
  const b = $("#btn-theme");
  b.textContent = THEME_TEXT[t];
  b.setAttribute("aria-label", t === "auto" ? "Colours: follow the system" : `Colours: ${t}`);
  try { localStorage.setItem(THEME_KEY, t); } catch (e) { /* private window */ }
}

// --- files ------------------------------------------------------------------------

async function loadDir(path) {
  const link = await needLink();
  const ents = await link.ls(path);
  S.cwd = path;
  ents.sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name) : a.type === "d" ? -1 : 1));
  const crumbs = $("#crumbs");
  crumbs.textContent = "";
  crumbs.append(el("button", { onclick: () => loadDir("/").catch(fail) }, "/"));
  let acc = "";
  for (const part of path.split("/").filter(Boolean)) {
    acc += "/" + part;
    const to = acc;
    crumbs.append(el("button", { onclick: () => loadDir(to).catch(fail) }, part), el("span", {}, "/"));
  }
  const rows = $("#file-rows");
  rows.textContent = "";
  if (!ents.length) rows.append(el("tr", {}, el("td", { colspan: "3", class: "empty" }, "This folder is empty.")));
  for (const e of ents) {
    const full = joinPath(path, e.name);
    // The device refuses to move or remove these; no point offering it.
    const fixed = SYSTEM_DIRS.has(full);
    const name = e.type === "d"
      ? el("button", { class: "link name dir", onclick: () => loadDir(full).catch(fail) }, e.name)
      : el("span", { class: "name" }, e.name);
    rows.append(el("tr", {},
      el("td", {}, name),
      el("td", { class: "num" }, e.type === "d" ? "" : human(e.size)),
      el("td", { class: "acts" },
        e.type === "d" ? null : el("button", { onclick: () => downloadFile(full, e.size).catch(fail), title: "Download to this computer" }, "Download"),
        " ",
        fixed ? null : el("button", { onclick: () => renameEntry(full).catch(fail) }, "Rename"),
        " ",
        fixed ? null : el("button", { class: "danger", onclick: () => deleteEntry(full, e.type === "d").catch(fail) }, "Delete"))));
  }
  renderPrompt();
}

function transferRow(label, list = "#transfers") {
  const prog = el("progress", { max: "1", value: "0" });
  const state = el("span", { class: "state" }, "waiting");
  const ctl = new AbortController();
  const cancel = el("button", { onclick: () => ctl.abort() }, "Cancel");
  const li = el("li", {}, el("span", { class: "what" }, label), el("span", {}, state, " ", cancel), prog);
  $(list).prepend(li);
  return {
    signal: ctl.signal,
    progress: (done, total) => {
      prog.max = total || 1;
      prog.value = done;
      state.textContent = `${human(done)} of ${human(total)}`;
    },
    finish: (text, bad = false) => {
      state.textContent = text;
      state.className = "state" + (bad ? " bad" : "");
      cancel.remove();
      setTimeout(() => li.remove(), bad ? 15000 : 4000);
    },
  };
}

async function uploadFiles(files) {
  const link = await needLink();
  const dir = S.cwd;
  for (const f of files) {
    const t = transferRow(`${f.name} to ${dir}`);
    try {
      const data = new Uint8Array(await f.arrayBuffer());
      await link.put(data, joinPath(dir, f.name), { progress: t.progress, signal: t.signal });
      t.finish("sent, SHA-256 checked");
    } catch (e) {
      t.finish(t.signal.aborted ? "cancelled" : explain(e), true);
    }
  }
  if (S.pane === "files" && S.cwd === dir) await loadDir(dir);
}

async function downloadFile(path, size) {
  const link = await needLink();
  const t = transferRow(`${path} to this computer`);
  try {
    const data = await link.get(path, { progress: t.progress, signal: t.signal });
    const url = URL.createObjectURL(new Blob([data]));
    const a = el("a", { href: url, download: path.split("/").pop() });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    t.finish("received, SHA-256 checked");
  } catch (e) {
    t.finish(t.signal.aborted ? "cancelled" : explain(e), true);
  }
}

// A file command in /os or /etc runs with sudo, as a PUT there does: the device
// asks for it, and the password given once answers both.
const isSystem = (p) => p === "/os" || p === "/etc" || p.startsWith("/os/") || p.startsWith("/etc/");

async function runChecked(line, ...paths) {
  const link = await needLink();
  if (paths.some(isSystem)) line = link.sudoLine(line);
  const r = await link.exec(line);
  if (r.rc !== 0) throw new RpcError(r.text.trim() || `${line} failed`);
  return r;
}

async function deleteEntry(path, isDir) {
  if (path.includes("*")) throw new RpcError("rm on the device treats * as a wildcard, so this page will not remove a name that has one. Use the Terminal.");
  const yes = await ask({
    title: `Delete ${path.split("/").pop()}?`,
    body: isDir ? `${path} and everything in it will be removed from the device.` : `${path} will be removed from the device.`,
    ok: "Delete", danger: true,
  });
  if (!yes) return;
  await runChecked(`rm ${isDir ? "-r " : ""}${quote(path)}`, path);
  await loadDir(S.cwd);
}

async function renameEntry(path) {
  const old = path.split("/").pop();
  const name = await ask({ title: `Rename ${old}`, input: old, ok: "Rename" });
  if (!name || name === old) return;
  if (name.includes("/")) throw new RpcError("A name cannot contain /.");
  await runChecked(`mv ${quote(path)} ${quote(joinPath(parentPath(path), name))}`, path);
  await loadDir(S.cwd);
}

async function newFolder() {
  const name = await ask({ title: "New folder", body: `In ${S.cwd}`, input: "", ok: "Create" });
  if (!name) return;
  if (name.includes("/")) throw new RpcError("A name cannot contain /.");
  await runChecked("mkdir " + quote(joinPath(S.cwd, name)), joinPath(S.cwd, name));
  await loadDir(S.cwd);
}

// --- run --------------------------------------------------------------------------

const HISTORY_KEY = "rpc-web-history";
let history = [];
try { history = JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]"); } catch (e) { history = []; }
let histPos = -1;

function renderHistory() {
  const ul = $("#run-history");
  ul.textContent = "";
  for (const h of history.slice(0, 30)) {
    ul.append(el("li", {}, el("button", { onclick: () => { $("#run-input").value = h; $("#run-input").focus(); } }, h)));
  }
}

async function runCommand(line) {
  if (!line.trim()) return;
  history = [line, ...history.filter((h) => h !== line)].slice(0, 50);
  try { localStorage.setItem(HISTORY_KEY, JSON.stringify(history)); } catch (e) { /* private window */ }
  histPos = -1;
  renderHistory();
  const status = $("#run-status");
  const out = $("#run-out");
  status.textContent = "Running...";
  out.textContent = "";
  const ctl = new AbortController();
  S.runAbort = ctl;
  $("#run-stop").disabled = false;
  $("#run-go").disabled = true;
  try {
    const link = await needLink();
    const r = await link.exec(line, { signal: ctl.signal });
    out.textContent = r.text;
    status.textContent = "";
    status.append(el("span", { class: "rc " + (r.rc === 0 ? "good" : "bad") }, `rc ${r.rc}`), " ",
      r.rc === 0 ? "succeeded" : "reported an error",
      r.stopped ? ", stopped with Ctrl+C" : "",
      r.truncated ? `; output cut off at ${human(link.execMax)}` : "");
  } catch (e) {
    status.textContent = explain(e);
  } finally {
    S.runAbort = null;
    $("#run-stop").disabled = true;
    $("#run-go").disabled = false;
  }
}

// --- device -----------------------------------------------------------------------

function renderFacts() {
  const dl = $("#facts");
  dl.textContent = "";
  const h = S.link ? S.link.hello : {};
  const rows = [["Firmware", h.os ? `${h.os} (build ${h.build})` : null], ["Board", h.board],
    ["Packages", S.link ? linkArch(h) : null], ["Account", h.user], ["Folder", h.cwd], ["Link protocol", h.proto]];
  for (const [k, v] of rows) if (v) dl.append(el("dt", {}, k), el("dd", {}, v));
  renderPrompt();
}

async function deviceCommand(cmd) {
  const link = await needLink();
  const r = await link.exec(cmd);
  $("#device-out").textContent = r.text;
}

async function reboot() {
  const yes = await ask({ title: "Restart the device?", body: "The connection drops while it restarts. Files are kept.", ok: "Restart", danger: true });
  if (!yes) return;
  const link = await needLink();
  try {
    const r = await link.exec("reboot", { timeout: 6000 });
    $("#device-out").textContent = r.text;      // still here: it was refused
  } catch (e) {
    if (S.line) throw e;                        // gone, which is what a restart looks like
  }
}

// --- start ------------------------------------------------------------------------

async function start() {
  if (DEMO) {
    const { demoSerial } = await import("./demo.js");
    S.serial = demoSerial();
    $("#connect-status").textContent = "Demo mode: a pretend device stands in for a real one. Choose device to connect to it.";
  } else if (webSerialSupported()) {
    S.serial = navigator.serial;
    S.serial.addEventListener("connect", () => { if (!S.line && S.port) $("#btn-reconnect").hidden = false; });
  } else {
    $("#unsupported").hidden = false;
    $("#btn-choose").disabled = true;
    const phone = (navigator.userAgentData && navigator.userAgentData.mobile) || /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent);
    if (phone) $("#unsupported-why").textContent = "Phones and tablets cannot open serial ports from a browser.";
    else if (!isSecureContext) $("#unsupported-why").textContent = "Serial ports are offered only to a secure page: open this one over https, or as http://localhost.";
  }

  let theme = "auto";
  try { theme = localStorage.getItem(THEME_KEY) || "auto"; } catch (e) { /* private window */ }
  setTheme(theme);
  $("#btn-theme").addEventListener("click", () => {
    const now = document.documentElement.dataset.theme || "auto";
    setTheme(THEMES[(THEMES.indexOf(now) + 1) % THEMES.length]);
  });

  // What the Firmware and Packages panes borrow from the page.
  const ctx = {
    el, ask, human, explain, toast, showError, transferRow, saveFile, demo: DEMO,
    releasesBase: new URLSearchParams(location.search).get("releases") || undefined,
    link: () => (S.mode === "link" && S.link && !S.link.dropped ? S.link : null),
    line: () => S.line,
    connected: () => !!S.line,
    knownPort: () => !!S.port,
    needLink,
    waitForDevice,
    console: consoleFor,
    bootsel: bootselNow,
  };
  S.fw = firmwarePane(ctx);
  S.pk = packagesPane(ctx);

  for (const b of document.querySelectorAll(".tabs button")) {
    b.tabIndex = b.getAttribute("aria-selected") === "true" ? 0 : -1;
    b.addEventListener("click", () => showPane(b.dataset.pane));
  }
  $(".tabs").addEventListener("keydown", tabKeys);
  $("#btn-choose").addEventListener("click", choosePort);
  $("#btn-reconnect").addEventListener("click", reconnect);
  $("#btn-disconnect").addEventListener("click", () => disconnect().catch(fail));
  $("#btn-bootsel").addEventListener("click", () => bootsel().catch(fail));
  $("#btn-ctrlc").addEventListener("click", () => { if (S.line && S.mode !== "link") S.line.write(new Uint8Array([3])).catch(fail); });
  $("#btn-clear").addEventListener("click", () => { if (S.term) S.term.clear(); });

  $("#btn-up").addEventListener("click", () => loadDir(parentPath(S.cwd)).catch(fail));
  $("#btn-refresh").addEventListener("click", () => loadDir(S.cwd).catch(fail));
  $("#btn-mkdir").addEventListener("click", () => newFolder().catch(fail));
  $("#btn-upload").addEventListener("click", () => $("#upload-input").click());
  $("#upload-input").addEventListener("change", (e) => { const f = [...e.target.files]; e.target.value = ""; uploadFiles(f).catch(fail); });
  const drop = $("#drop");
  drop.addEventListener("dragover", (e) => { e.preventDefault(); drop.classList.add("over"); });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => {
    e.preventDefault();
    drop.classList.remove("over");
    const items = [...(e.dataTransfer.items || [])];
    const folders = items.filter((i) => i.webkitGetAsEntry && i.webkitGetAsEntry() && i.webkitGetAsEntry().isDirectory).length;
    if (folders) toast("Folders are not uploaded from here; drop the files inside them, or use rpc put -r.");
    const files = [...e.dataTransfer.files].filter((f) => f.size > 0 || f.type);
    if (files.length) uploadFiles(files).catch(fail);
  });

  $("#run-form").addEventListener("submit", (e) => { e.preventDefault(); runCommand($("#run-input").value); });
  $("#run-stop").addEventListener("click", () => { if (S.runAbort) S.runAbort.abort(); });
  $("#run-input").addEventListener("keydown", (e) => {
    if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
    e.preventDefault();
    histPos = Math.max(-1, Math.min(history.length - 1, histPos + (e.key === "ArrowUp" ? 1 : -1)));
    $("#run-input").value = histPos < 0 ? "" : history[histPos];
  });
  renderHistory();

  for (const b of document.querySelectorAll("#pane-device [data-cmd]")) b.addEventListener("click", () => deviceCommand(b.dataset.cmd).catch(fail));
  $("#btn-reboot").addEventListener("click", () => reboot().catch(fail));

  // Leaving the page: say goodbye if we can. If not, the device leaves link
  // mode on its own three seconds after the port closes.
  addEventListener("pagehide", () => { if (S.line && S.mode === "link") S.line.write(enc.encode("BYE\n")).catch(() => {}); });
  setMode("off");

  // Last, because it may wait on the CDN; until it is ready the device's
  // output is held for it (see termWrite).
  const term = await createTerminal($("#term"), (bytes) => { if (S.line && S.mode !== "link" && S.mode !== "switching") S.line.write(bytes).catch(fail); });
  if (term.kind === "plain") $("#term-note").textContent = `A plain terminal is in use: xterm.js could not be loaded (${term.reason}).`;
  S.term = term;
  if (pending.length) { for (const b of pending) term.write(b); pending.length = 0; }
}

start().catch(fail);
