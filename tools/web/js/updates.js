// Firmware updates: what the latest release is, where this device stands
// against it, and the three ways of getting it onto the board.
//
//   over the air     `sudo update online --yes` through link's EXEC; the
//                    device downloads, checks and writes the image itself
//   from this page   the browser fetches the .bin, sends it to /os/update.bin
//                    with link's PUT, then `sudo update from-file` at the console
//   BOOTSEL          the .uf2 written onto the bootloader's drive, or saved
//                    for dragging there by hand
//
// The top half has no DOM and runs in the tests (test/updates.test.js); the
// pane at the bottom is the page's.

import { LinkError, RpcError, DigestError, sha256hex, stripAnsi } from "./link.js";
import { verdict, deviceVersion, buildOf, versionCmp } from "./version.js";

export const RELEASES_DEFAULT = "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/";

// update.h: where `update install` stages its download. `update from-file`
// takes any path, but this one is removed by the update itself before it saves
// the copy of the running firmware, which a 1.8 MB filesystem needs the room for.
export const IMAGE_PATH = "/os/update.bin";
export const ROLLBACK_IMG = "/os/rollback.img";

export const CHIP = { pico2_w: "RP2350", pico2: "RP2350", pico_w: "RP2040", pico: "RP2040" };
export const RADIO = { pico2_w: true, pico_w: true, pico2: false, pico: false };
export const BOARD_NAMES = { pico2_w: "Pico 2 W", pico2: "Pico 2", pico_w: "Pico W", pico: "Pico" };

const enc = new TextEncoder();
const dec = new TextDecoder();

// --- the release files ---------------------------------------------------------

// latest.json: one "packages" entry per board, the shape of a package index
// entry, which is how the device reads it (repo_find by board name).
export function parseManifest(doc) {
  const list = doc && Array.isArray(doc.packages) ? doc.packages : null;
  if (!list) throw new RpcError("latest.json has no list of images");
  return list.filter((p) => p && p.name && p.ver).map((p) => ({
    name: String(p.name), ver: String(p.ver), desc: String(p.desc || ""), arch: String(p.arch || ""),
    size: Number(p.size || 0), sha256: String(p.sha256 || "").toLowerCase(), url: String(p.url || ""),
  }));
}

export function manifestFor(entries, board) {
  if (!entries || !board) return null;
  return entries.find((e) => e.name.toLowerCase() === String(board).toLowerCase()) || null;
}

// The three files, each on its own, so one missing still shows the others.
export async function loadReleases(base = RELEASES_DEFAULT, fetchFn = globalThis.fetch) {
  const get = async (name) => {
    let r;
    try { r = await fetchFn(base + name, { cache: "no-store" }); } catch (e) {
      throw new RpcError(`could not fetch ${name} (${e.message || e})`);
    }
    if (!r.ok) throw new RpcError(`${name} answered HTTP ${r.status}`);
    try { return await r.json(); } catch (e) { throw new RpcError(`${name} is not JSON`); }
  };
  const out = { manifest: null, history: null, boards: null, errors: {} };
  const [m, h, b] = await Promise.allSettled([get("latest.json"), get("releases.json"), get("boards.json")]);
  try { if (m.status === "fulfilled") out.manifest = parseManifest(m.value); else out.errors.manifest = m.reason.message; } catch (e) { out.errors.manifest = e.message; }
  if (h.status === "fulfilled") out.history = Array.isArray(h.value && h.value.releases) ? h.value.releases : [];
  else out.errors.history = h.reason.message;
  if (b.status === "fulfilled") out.boards = Array.isArray(b.value && b.value.boards) ? b.value.boards : [];
  else out.errors.boards = b.reason.message;
  return out;
}

export function releaseFor(history, build) {
  return (history || []).find((r) => Number(r.build) === Number(build)) || null;
}

// The images at a fixed name beside latest.json. Fetched from there rather
// than from the GitHub release, whose downloads send no CORS headers.
export function imageUrl(base, board, kind) { return `${base}rpcortex-v2-${board}.${kind}`; }

// What the .uf2 must hash to: the release whose build latest.json names.
export function uf2Expect(history, board, build) {
  const r = releaseFor(history, build);
  const a = r && (r.assets || []).find((x) => x.board === board && x.kind === "uf2");
  return a ? { sha256: String(a.sha256 || "").toLowerCase(), size: Number(a.size || 0), release: r } : null;
}

// Bytes from a URL with progress, refused unless they match `sha256` when one
// is given. Resolves to { data, verified }.
export async function fetchBytes(url, { sha256, size, progress, signal, fetchFn = globalThis.fetch } = {}) {
  let r;
  try { r = await fetchFn(url, { cache: "no-store", signal }); } catch (e) {
    if (signal && signal.aborted) throw new RpcError("cancelled");
    throw new RpcError(`could not fetch ${url.split("/").pop()} (${e.message || e})`);
  }
  if (!r.ok) throw new RpcError(`${url.split("/").pop()} answered HTTP ${r.status}`);
  const total = Number(r.headers && r.headers.get && r.headers.get("content-length")) || size || 0;
  let data;
  if (r.body && r.body.getReader) {
    const reader = r.body.getReader();
    const parts = [];
    let got = 0;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      if (progress) progress(got, total || got);
    }
    data = new Uint8Array(got);
    let o = 0;
    for (const p of parts) { data.set(p, o); o += p.length; }
  } else {
    data = new Uint8Array(await r.arrayBuffer());
    if (progress) progress(data.length, data.length);
  }
  if (!sha256) return { data, verified: false };
  const have = await sha256hex(data);
  if (have !== sha256) {
    throw new DigestError(`${url.split("/").pop()} does not match the release (expected ${sha256}, received ${have}). Nothing was sent.`);
  }
  return { data, verified: true };
}

// --- what an image is ---------------------------------------------------------

// The chip an image is built for, from where its vector table sits: at 0 on
// RP2350, after the 256-byte second stage at 0x100 on RP2040. update.cpp
// accepts both places, so an image for the other chip would pass its check
// and leave the board needing BOOTSEL. The page checks first.
export function binChip(data) {
  if (!data || data.length < 0x108) return null;
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const sane = (off) => {
    const sp = v.getUint32(off, true), entry = v.getUint32(off + 4, true);
    return sp >= 0x20000000 && sp < 0x20090000 && entry >= 0x10000000 && entry < 0x10200000;
  };
  if (sane(0)) return "RP2350";
  if (sane(0x100)) return "RP2040";
  return null;
}

// The version and variant an RPCortex image says it is, if it says.
export function sniffImage(data) {
  const n = Math.min(data.length, 2 << 20);
  let text = "";
  for (let i = 0; i < n; i += 0x8000) text += String.fromCharCode.apply(null, data.subarray(i, Math.min(n, i + 0x8000)));
  const v = /v2\.\d+(?:\.\d+)?(?:-[a-z0-9]+)?/.exec(text);
  const k = /rpcimage:([a-z0-9-]+)/.exec(text);
  return { version: v ? v[0] : null, variant: k ? k[1] : null, chip: binChip(data) };
}

const UF2_MAGIC = [0x0a324655, 0x9e5d5157];
const FAMILY = { 0xe48bff56: "RP2040", 0xe48bff59: "RP2350", 0xe48bff5a: "RP2350", 0xe48bff5b: "RP2350" };

// The chip a .uf2 is for, from its blocks' family IDs. The first block of an
// RP2350 image is an "absolute" one (0xe48bff57) that both chips accept, so
// that one is skipped.
export function uf2Chip(data) {
  if (!data || data.length < 512 || data.length % 512) return null;
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
  for (let o = 0; o < data.length && o < 512 * 8; o += 512) {
    if (v.getUint32(o, true) !== UF2_MAGIC[0] || v.getUint32(o + 4, true) !== UF2_MAGIC[1]) return null;
    const flags = v.getUint32(o + 8, true);
    if (!(flags & 0x2000)) continue;
    const chip = FAMILY[v.getUint32(o + 28, true)];
    if (chip) return chip;
  }
  return null;
}

// The chip a BOOTSEL drive belongs to, from its INFO_UF2.TXT:
// "Board-ID: RPI-RP2" on RP2040, "Board-ID: RP2350" on RP2350.
// DEVICE-UNCONFIRMED: read from the bootloader's documented file, not yet
// from a drive through this page.
export function bootselChip(info) {
  const id = /Board-ID:\s*(\S+)/i.exec(info || "");
  if (id && /RP2350/i.test(id[1])) return "RP2350";
  if (id && /RPI-RP2|RP2040/i.test(id[1])) return "RP2040";
  if (/RP2350/i.test(info || "")) return "RP2350";
  if (/RP2040|RPI-RP2/i.test(info || "")) return "RP2040";
  return null;
}

// Write a .uf2 onto a BOOTSEL drive chosen with showDirectoryPicker. The boot
// ROM takes the blocks as they land, whatever the file is called, flashes them
// and restarts, so the drive can vanish before the browser has finished
// closing the file. An error at that point, after every byte was written, is
// what success looks like; it is reported as `vanished`.
// DEVICE-UNCONFIRMED: whether Chrome lets a page pick the root of a removable
// drive, and how its write lands there, only a real browser and board can show.
export async function writeUf2(dir, data, { chip } = {}) {
  let info = null;
  try { info = await (await (await dir.getFileHandle("INFO_UF2.TXT")).getFile()).text(); } catch (e) { info = null; }
  if (info === null) {
    throw new RpcError(`"${dir.name}" is not a BOOTSEL drive: it has no INFO_UF2.TXT. Choose the drive named RP2350 or RPI-RP2.`);
  }
  const drive = bootselChip(info);
  const image = uf2Chip(data);
  if (!image) throw new RpcError("That is not a UF2 file.");
  if (drive && image !== drive) {
    throw new RpcError(`This .uf2 is for the ${image}, and the drive belongs to an ${drive}. Nothing was written.`);
  }
  if (chip && image !== chip) {
    throw new RpcError(`This .uf2 is for the ${image}, and the board was an ${chip}. Nothing was written.`);
  }
  const fh = await dir.getFileHandle("rpcortex.uf2", { create: true });
  const w = await fh.createWritable();
  let written = false;
  try {
    await w.write(data);
    written = true;
    await w.close();
    return { vanished: false };
  } catch (e) {
    if (written) return { vanished: true };
    try { await w.abort(); } catch (e2) { /* the drive went with it */ }
    throw e;
  }
}

// --- over the air -------------------------------------------------------------

// --yes because EXEC cannot answer the "Continue?" that update_online asks:
// without it the command would wait for ever for keys nobody sends.
export function otaLine(link, force) {
  return link.sudoLine("update online " + (force ? "--force " : "") + "--yes");
}

// Run the update over the air. Resolves to
//   { restarted: true }                      the device went away to write it
//   { restarted: false, rc, text, upToDate, refused, canForce }
// `gone()` says whether the port has closed, which is what a successful update
// looks like from here: the device never sends the final frame, because it
// restarts in the middle of the command.
export async function otaUpdate(link, { force = false, gone = () => false } = {}) {
  // Asked before the long wait, not after it (PERMISSIONS.md: rpc --sudo).
  if (link.hasSudo) await link.authenticate();
  let r;
  try {
    r = await link.exec(otaLine(link, force));
  } catch (e) {
    if (gone()) return { restarted: true };
    throw e;
  }
  const text = r.text || "";
  return {
    restarted: false, rc: r.rc, text,
    upToDate: /Already (running|up to date)/i.test(text),
    canForce: !force && /--force/.test(text),
    refused: r.rc !== 0,
  };
}

// --- from this page -------------------------------------------------------------

async function mustExec(link, line) {
  const r = await link.exec(line);
  if (r.rc !== 0) throw new RpcError((r.text || "").trim() || `${line} failed`);
  return r;
}

// Put an image at IMAGE_PATH. A file already there is removed first, since
// PUT counts it as used while it replaces it. When there is no room and the
// copy of the previous firmware is what fills it, `confirmForget(bytes)` is
// asked, and on yes it goes (as update_online does for its own download).
// `authed` says the caller has just sent AUTH itself, so sudo is not asked
// for a second time; if the grace runs out on the way, it is asked then.
export async function sendImage(link, data, { progress, signal, confirmForget, authed = false } = {}) {
  if (link.hasSudo && !authed) await link.authenticate();
  if (await link.stat(IMAGE_PATH)) await mustExec(link, link.sudoLine("rm " + IMAGE_PATH));
  const put = () => link.put(data, IMAGE_PATH, { progress, signal });
  try {
    return await put();
  } catch (e) {
    if (!(e instanceof LinkError && e.code === "nospace")) throw e;
    const saved = await link.stat(ROLLBACK_IMG);
    const kb = Math.round(data.length / 1024);
    if (!saved) throw new RpcError(`The device has no room for the ${kb} KB image (${e.detail}). 'df' shows what is using it.`);
    if (!confirmForget || !(await confirmForget(saved.size))) {
      throw new RpcError(`The device has no room for the ${kb} KB image while it keeps the copy of its previous firmware. Nothing was changed.`);
    }
    await mustExec(link, link.sudoLine("update rollback --forget"));
    try {
      return await put();
    } catch (e2) {
      if (e2 instanceof LinkError && e2.code === "nospace") {
        throw new RpcError(`The device still has no room for the ${kb} KB image (${e2.detail}). 'df' shows what is using it.`);
      }
      throw e2;
    }
  }
}

// `update from-file` always stops to ask "Continue? (yes/no)" and has no
// --yes, so it cannot run through EXEC. It is typed at the console instead,
// where the page answers the password prompt sudo shows there (link mode's
// grace ends with link mode) and the question, and passes everything the
// device prints to `echo` so the person sees it happen.
export const FROM_FILE = "update from-file " + IMAGE_PATH;

const SHELL_PROMPT = /\S+@\S+:.*>\s*$/;
const lastLine = (t) => t.replace(/\r\n/g, "\n").split("\n").pop().split("\r").pop();

async function typeLine(stream, text) {
  for (const b of enc.encode(text)) await stream.write(new Uint8Array([b]));
  await stream.write(enc.encode("\r"));
}

// Type `line` at the shell's prompt and answer what it asks. Resolves to
//   { restarted: true, text }    the port closed after the question was
//                                answered: the device is writing the image
//   { restarted: false, text }   the prompt came back: it did not happen
// `password` answers sudo's prompt once; a second prompt means it was wrong.
// DEVICE-UNCONFIRMED: driven against the mock's console, not yet a board's.
export async function runAtConsole(stream, line, { password, echo, timeout = 20000, settle = 300, after = 300000 } = {}) {
  const decoder = new TextDecoder();
  let text = "";
  const feed = (b) => { if (echo) echo(b); text += stripAnsi(decoder.decode(b, { stream: true })); };
  const read = async (ms) => {
    try { return await stream.read(ms); } catch (e) { return null; }
  };
  // Whatever was half typed goes first, and what came before is not ours.
  await stream.write(new Uint8Array([3]));
  for (const end = Date.now() + settle; Date.now() < end;) {
    const d = await read(50);
    if (d === null) return { restarted: false, text, lost: true };
    if (d.length && echo) echo(d);
  }
  text = "";
  await typeLine(stream, line);
  let from = 0;
  let passwords = 0;
  let answered = false;
  let deadline = Date.now() + timeout;
  for (;;) {
    if (Date.now() > deadline) {
      throw new RpcError(answered ? "The device has not restarted. Look at the Terminal." : "The device did not ask what was expected. Look at the Terminal.");
    }
    const d = await read(200);
    if (d === null) {
      if (answered) return { restarted: true, text };
      return { restarted: false, text, lost: true };
    }
    if (!d.length) continue;
    feed(d);
    const tail = text.slice(from);
    const last = lastLine(tail);
    if (/Password for/.test(last)) {
      if (!password || passwords >= 1) {
        // Empty answers until sudo gives up, so the prompt is left as it was.
        for (let i = 0; i < 3; i++) await typeLine(stream, "");
        throw new RpcError(password ? "The device did not take the password." : "The device asked for a password.");
      }
      passwords++;
      await typeLine(stream, password);
      from = text.length;
    } else if (/\(yes\/no\)/.test(last)) {
      await typeLine(stream, "yes");
      answered = true;
      deadline = Date.now() + after;
      from = text.length;
    } else if (tail.includes("\n") && SHELL_PROMPT.test(last)) {
      return { restarted: false, text };
    }
  }
}

// --- the pane ---------------------------------------------------------------------

// Everything below touches the page and runs only in a browser. `ctx` is what
// app.js lends it: see app.js where firmwarePane is called.

const NOTE_SECTIONS = [["new", "New"], ["improved", "Improved"], ["fixed", "Fixed"], ["security", "Security"], ["changed", "Changed"], ["issues", "Known issues"]];

// `code` and **bold** from the notes, as elements, never as HTML.
function inline(el, text) {
  const out = [];
  for (const part of String(text).split(/(`[^`]+`|\*\*[^*]+\*\*)/)) {
    if (!part) continue;
    if (part.startsWith("`") && part.endsWith("`") && part.length > 2) out.push(el("code", {}, part.slice(1, -1)));
    else if (part.startsWith("**") && part.endsWith("**") && part.length > 4) out.push(el("strong", {}, ...inline(el, part.slice(2, -2))));
    else out.push(document.createTextNode(part));
  }
  return out;
}

// A list of steps, one active at a time, with the time it has taken.
function stepList(el, ol, labels) {
  ol.textContent = "";
  ol.hidden = false;
  const items = labels.map((l) => {
    const detail = el("span", { class: "detail" });
    const li = el("li", { class: "step" }, el("span", { class: "label" }, l), detail);
    ol.append(li);
    return { li, detail };
  });
  let timer = null;
  const stop = () => { if (timer) clearInterval(timer); timer = null; };
  const set = (i, state, text) => {
    const it = items[i];
    it.li.className = "step " + state;
    if (state === "active") it.li.setAttribute("aria-current", "step"); else it.li.removeAttribute("aria-current");
    it.detail.textContent = text || "";
  };
  return {
    start(i, text) {
      stop();
      set(i, "active", text);
      const t0 = Date.now();
      timer = setInterval(() => {
        const s = Math.round((Date.now() - t0) / 1000);
        items[i].detail.textContent = (text ? text + " " : "") + `(${s} s)`;
      }, 1000);
    },
    done(i, text) { stop(); set(i, "done", text); },
    fail(i, text) { stop(); set(i, "failed", text); },
    stop,
  };
}

// The console output as a person would see it: colour dropped, and a
// carriage return redrawing its line the way a progress bar expects.
// A CR followed by LF is an ordinary line end; a CR followed by anything else
// starts its line again. The two can arrive in different reads.
export function consoleText() {
  const decoder = new TextDecoder();
  let lines = [""];
  let cr = false;
  return {
    clear() { lines = [""]; cr = false; },
    feed(bytes) {
      const t = stripAnsi(decoder.decode(bytes, { stream: true })).replace(/\x1e[^\n]*\n?/g, "");
      for (const ch of t) {
        if (ch === "\r") { cr = true; continue; }
        if (ch === "\n") { lines.push(""); cr = false; continue; }
        if (cr) { lines[lines.length - 1] = ""; cr = false; }
        if (ch === "\b") lines[lines.length - 1] = lines[lines.length - 1].slice(0, -1);
        else lines[lines.length - 1] += ch;
      }
      if (lines.length > 400) lines = lines.slice(-400);
    },
    text() { return lines.join("\n"); },
  };
}

function consoleView(pre) {
  const c = consoleText();
  return {
    clear() { c.clear(); pre.textContent = ""; },
    write(bytes) {
      c.feed(bytes);
      pre.hidden = false;
      pre.textContent = c.text();
      pre.scrollTop = pre.scrollHeight;
    },
  };
}

export function firmwarePane(ctx) {
  const { el, ask, human, explain } = ctx;
  const $ = (s) => document.querySelector(s);
  const base = ctx.releasesBase || RELEASES_DEFAULT;
  const P = { rel: null, loading: null, board: null, busy: false, expecting: false, uf2: null };
  const view = consoleView($("#fw-console"));

  const error = (e) => ctx.showError("firmware", e);
  const hello = () => (ctx.link() ? ctx.link().hello : null);
  const board = () => (hello() && hello().board) || $("#fw-board").value || "pico2_w";

  function busy(on) {
    P.busy = on;
    for (const b of document.querySelectorAll("#pane-firmware [data-fw]")) b.disabled = on || b.dataset.off === "1";
  }

  async function load(force) {
    if (P.rel && !force) return P.rel;
    if (!P.loading) {
      $("#fw-latest").textContent = "Looking for the latest release...";
      P.loading = loadReleases(base).finally(() => { P.loading = null; });
    }
    P.rel = await P.loading;
    return P.rel;
  }

  // --- what is shown ---------------------------------------------------------

  function renderStatus() {
    const R = P.rel || {};
    const h = hello();
    const b = board();
    const sel = $("#fw-board");
    sel.value = b;
    sel.disabled = !!h;
    $("#fw-board-row").hidden = !!h;
    const entry = manifestFor(R.manifest, b);
    const rel = entry && releaseFor(R.history, buildOf(entry.ver));
    const name = rel ? rel.display : entry ? `v${entry.ver}` : null;
    const facts = $("#fw-facts");
    facts.textContent = "";
    const row = (k, v) => { if (v) facts.append(el("dt", {}, k), el("dd", {}, v)); };
    const mine = h ? deviceVersion(h) : null;
    row("This device", mine ? `${mine.display}${mine.newScheme ? "" : ` (build ${mine.build})`} on ${BOARD_NAMES[b] || b}` : null);
    row("Latest release", name ? `${name}${rel && rel.date ? `, ${rel.date}` : ""}` : null);
    if (entry && entry.size) row("Image", `${human(entry.size)} for the ${BOARD_NAMES[b] || b}`);
    $("#fw-latest").textContent = R.errors && R.errors.manifest
      ? `The latest release could not be read: ${R.errors.manifest}.`
      : !entry && R.manifest ? `The latest release has no image for the ${b} board.` : "";

    const v = h && entry ? verdict(h, entry) : { state: h ? "unknown" : "none" };
    const note = $("#fw-verdict");
    note.textContent = "";
    note.className = "notice";
    const say = (cls, ...kids) => { note.className = "notice " + cls; note.append(...kids); note.hidden = false; };
    if (!h) {
      note.hidden = false;
      note.className = "notice info";
      note.append(ctx.connected() ? "Reading the device..." : "Connect a device to see what it runs. The release and its images are here either way.");
    } else if (v.state === "update") say("good", el("strong", {}, `${name} is available.`), ` This device runs ${mine.display}.`);
    else if (v.state === "current") say("info", el("strong", {}, "Up to date."), ` This device runs ${name}.`);
    else if (v.state === "ahead") say("info", el("strong", {}, "Newer than the latest release."), ` This device runs build ${mine.build}, a build of its own; the release is build ${v.latestBuild}.`);
    else if (v.state === "force") {
      say("warn", el("strong", {}, `${name} is newer, but this device will not offer it.`), " ",
        `${mine.display} numbered its versions the old way, so it reads ${entry.ver} as older than ${mine.cmp}. Updating once with --force moves it across; after that, updates are offered as usual.`);
    } else note.hidden = true;

    const wifi = RADIO[b] !== false;
    const ota = $("#fw-ota");
    ota.dataset.off = !h || !entry || !wifi ? "1" : "0";
    ota.textContent = v.state === "force" ? "Update over the air (--force)" : v.state === "current" || v.state === "ahead" ? "Reinstall over the air" : "Update over the air";
    $("#fw-ota-hint").textContent = !wifi
      ? `The ${BOARD_NAMES[b] || b} has no Wi-Fi, so use one of the other two ways.`
      : "The device downloads the image over its own Wi-Fi, checks it against the release, and writes it. It needs to be on a network (wifi connect).";
    $("#fw-send").dataset.off = !h || !entry ? "1" : "0";
    $("#fw-send-file").dataset.off = !h ? "1" : "0";
    $("#fw-uf2").dataset.off = !entry ? "1" : "0";
    $("#fw-uf2-write").dataset.off = !entry ? "1" : "0";
    $("#fw-uf2-write").hidden = typeof globalThis.showDirectoryPicker !== "function";
    $("#fw-uf2-hint").textContent = typeof globalThis.showDirectoryPicker === "function"
      ? "Reset to BOOTSEL, then choose the drive that appears and the page writes the image onto it. Or save the .uf2 and drag it there yourself."
      : "This browser cannot write to a drive, so save the .uf2, reset the board to BOOTSEL, and drag the file onto the drive named RP2350 or RPI-RP2.";
    busy(P.busy);
  }

  function renderNotes() {
    const box = $("#fw-notes");
    box.textContent = "";
    const R = P.rel || {};
    if (R.errors && R.errors.history) { box.append(el("p", { class: "hint" }, `The release notes could not be read: ${R.errors.history}.`)); return; }
    const list = (R.history || []).slice(0, 8);
    if (!list.length) { box.append(el("p", { class: "hint" }, "No releases yet.")); return; }
    list.forEach((r, i) => {
      const body = el("div", { class: "notes" });
      if (r.summary) body.append(el("p", {}, ...inline(el, r.summary.charAt(0).toUpperCase() + r.summary.slice(1))));
      for (const [key, label] of NOTE_SECTIONS) {
        const items = (r.notes && r.notes[key]) || [];
        if (!items.length) continue;
        body.append(el("h3", {}, label), el("ul", {}, ...items.map((t) => el("li", {}, ...inline(el, t)))));
      }
      const d = el("details", { class: "release", open: i === 0 },
        el("summary", {}, el("span", { class: "rel-name" }, r.display || r.tag || "?"), " ",
          el("span", { class: "hint" }, [r.codename, r.date, r.prerelease ? "pre-release" : null].filter(Boolean).join(", "))),
        body);
      box.append(d);
    });
  }

  function renderBoards() {
    const rows = $("#fw-board-rows");
    rows.textContent = "";
    const R = P.rel || {};
    if (R.errors && R.errors.boards) {
      rows.append(el("tr", {}, el("td", { colspan: "4", class: "empty" }, `Board status could not be read: ${R.errors.boards}.`)));
      return;
    }
    const here = hello() ? hello().board : null;
    for (const b of R.boards || []) {
      const ran = b.status === "verified" ? `Run on hardware, build ${b.verified_build}` : "Not yet booted";
      rows.append(el("tr", { class: b.id === here ? "here" : "", "aria-current": b.id === here ? "true" : null },
        el("td", {}, el("span", { class: "name" }, b.name || b.id), b.id === here ? el("span", { class: "tag" }, "this one") : null),
        el("td", {}, `${b.chip || ""}${b.radio && b.radio !== "none" ? ", " + b.radio : ""}`),
        el("td", {}, el("span", { class: "status " + (b.status === "verified" ? "good" : "untested") }, ran)),
        el("td", { class: "wide" }, b.notes || "")));
    }
  }

  async function show() {
    $("#fw-board").value = board();
    renderStatus();
    try { await load(false); } catch (e) { error(e); }
    renderStatus();
    renderNotes();
    renderBoards();
  }

  // --- the device coming back ---------------------------------------------------

  async function welcomeBack(steps, i, want) {
    steps.start(i, "Waiting for the device to come back.");
    const back = await ctx.waitForDevice(240000);
    if (!back) {
      steps.fail(i, "It has not come back yet. When it is running again, choose Reconnect on the Connect tab.");
      return;
    }
    try {
      const link = await ctx.needLink();
      const now = deviceVersion(link.hello);
      const good = want && now && versionCmp(now.cmp, want.cmp) === 0;
      if (good) steps.done(i, `Now running ${now.display}.`);
      else if (now) steps.done(i, `Back, and running ${now.display}.`);
      else steps.done(i, "Back.");
    } catch (e) {
      steps.fail(i, `It is back, but link mode did not start: ${explain(e)}`);
    }
    renderStatus();
  }

  // --- over the air ----------------------------------------------------------------

  async function overTheAir(forceAsked) {
    const R = await load(false);
    const link = await ctx.needLink();
    const b = link.hello.board;
    const entry = manifestFor(R.manifest, b);
    if (!entry) throw new RpcError(`The latest release has no image for the ${b} board.`);
    const v = verdict(link.hello, entry);
    const force = forceAsked || v.state !== "update";
    const yes = await ask({
      title: force && v.state !== "force" ? `Reinstall ${entry.ver}?` : `Update to ${entry.ver}?`,
      body: "The device downloads the image itself, checks it, writes it over the firmware and restarts. Its files and settings are kept. While the firmware is written the USB connection drops for up to 30 seconds; leave it plugged in.",
      ok: "Update",
    });
    if (!yes) return;
    view.clear();
    $("#fw-console").hidden = true;
    const steps = stepList(el, $("#fw-steps"), ["Sudo", "Download and check, on the device", "Write the firmware and restart", "Back again"]);
    busy(true);
    try {
      steps.start(0);
      if (link.hasSudo) await link.authenticate();
      steps.done(0, link.hasSudo ? "Password accepted." : "Not needed on this firmware.");
      steps.start(1, `About ${human(entry.size || 0)}. The device says nothing until it is done, which usually takes a minute or two.`);
      const line = ctx.line();
      P.expecting = true;
      const r = await otaUpdate(link, { force, gone: () => !line || line.closed });
      if (!r.restarted) {
        P.expecting = false;
        view.write(enc.encode(r.text));
        if (r.upToDate) { steps.done(1, "The device says it already runs this release."); return; }
        steps.fail(1, "The device did not update. What it said is below.");
        if (r.canForce) {
          const again = await ask({ title: "Install it anyway?", body: "The device refused it, and says --force installs it anyway. Its reason is shown on the Firmware tab.", ok: "Install with --force", danger: true });
          if (again) { busy(false); await overTheAir(true); }
        }
        return;
      }
      steps.done(1, "Checked.");
      steps.start(2, "Writing. The connection drops while flash is written; this is normal. Do not unplug it.");
      await new Promise((r2) => setTimeout(r2, 1500));
      steps.done(2, "It restarted.");
      await welcomeBack(steps, 3, { cmp: entry.ver });
    } catch (e) {
      steps.stop();
      throw e;
    } finally {
      P.expecting = false;
      busy(false);
    }
  }

  // --- from this page ------------------------------------------------------------

  async function confirmForget(bytes) {
    return ask({
      title: "Make room for the image?",
      body: `The device keeps a copy of its previous firmware (${human(bytes)}) for update rollback, and the new image does not fit beside it. The update saves a fresh copy of what is running before it writes anything.`,
      ok: "Remove the old copy", danger: true,
    });
  }

  // The password for the console step: asked here, checked with AUTH while
  // link mode still runs, and kept only for the length of this update.
  async function password(link) {
    if (!link.hasSudo) return null;
    const who = link.hello.user || "this account";
    let wrong = false;
    for (let i = 0; i < 3; i++) {
      const pw = await ask({
        title: wrong ? `Wrong password. Password for ${who}` : `Password for ${who}`,
        body: "Writing firmware needs sudo. The page types it at the device's prompt for this one update and keeps it nowhere." + (ctx.demo ? " (The demo device's password is demo.)" : ""),
        input: "", ok: "Continue", secret: true,
      });
      if (!pw) throw new RpcError("Writing firmware needs sudo, which asks for the account's password.");
      try { await link.auth(pw); return pw; } catch (e) {
        if (!(e instanceof LinkError) || e.code !== "auth") throw e;
        wrong = true;
      }
    }
    throw new RpcError(`Three wrong passwords for ${who}; nothing was done.`);
  }

  async function fromPage(file) {
    const R = await load(false);
    let link = await ctx.needLink();
    const b = link.hello.board;
    const entry = manifestFor(R.manifest, b);
    let data = null, label;
    if (file) {
      data = new Uint8Array(await file.arrayBuffer());
      const s = sniffImage(data);
      if (s.chip && CHIP[b] && s.chip !== CHIP[b]) {
        throw new RpcError(`${file.name} is built for the ${s.chip}, and this board is an ${CHIP[b]}. Writing it would leave the board needing BOOTSEL, so it was not sent.`);
      }
      if (!s.chip) throw new RpcError(`${file.name} does not look like a firmware image (no vector table where one belongs).`);
      label = `${file.name}${s.version ? ` (${s.version}${s.variant && s.variant !== "standard" ? ", custom image" : ""})` : ""}`;
    } else {
      if (!entry) throw new RpcError(`The latest release has no image for the ${b} board.`);
      label = `v${entry.ver} for the ${BOARD_NAMES[b] || b}`;
    }
    const yes = await ask({
      title: `Write ${label}?`,
      body: (file ? "This file has no checksum to check it against, so it is written as it is. " : "") +
        "The image is sent to the device, which then writes it over its firmware and restarts. Files and settings are kept. The USB connection drops for up to 30 seconds while flash is written; leave it plugged in.",
      ok: "Write firmware", danger: !!file,
    });
    if (!yes) return;
    view.clear();
    $("#fw-console").hidden = true;
    const steps = stepList(el, $("#fw-steps"), [file ? "Read the file" : "Download and check, here", "Sudo", "Send it to the device", "Write the firmware, at the device's console", "Back again"]);
    busy(true);
    const ctl = new AbortController();
    try {
      steps.start(0);
      if (!file) {
        const got = await fetchBytes(entry.url || imageUrl(base, b, "bin"), {
          sha256: entry.sha256, size: entry.size, signal: ctl.signal,
          progress: (a, t) => steps.start(0, `${human(a)} of ${human(t)}`),
        });
        data = got.data;
        const chip = binChip(data);
        if (chip && CHIP[b] && chip !== CHIP[b]) throw new RpcError(`The release's ${b} image is built for the ${chip}. Nothing was sent.`);
      }
      steps.done(0, file ? human(data.length) : `${human(data.length)}, SHA-256 matches the release.`);
      steps.start(1);
      const pw = await password(link);
      steps.done(1, pw ? "Password accepted." : "Not needed on this firmware.");
      steps.start(2);
      await sendImage(link, data, {
        signal: ctl.signal, confirmForget, authed: !!pw,
        progress: (a, t) => steps.start(2, `${human(a)} of ${human(t)}`),
      });
      steps.done(2, `${IMAGE_PATH}, SHA-256 checked.`);
      steps.start(3, "The device's own output is below.");
      $("#fw-console").hidden = false;
      P.expecting = true;
      const con = await ctx.console();
      let r;
      try {
        r = await runAtConsole(con.stream, link.sudoLine(FROM_FILE), {
          password: pw, echo: (bytes) => { con.echo(bytes); view.write(bytes); },
        });
      } finally {
        con.release();
      }
      if (!r.restarted) {
        P.expecting = false;
        steps.fail(3, r.lost ? "The connection closed before the device was asked." : "The device did not write it. What it said is below.");
        return;
      }
      steps.done(3, "Written; it restarted.");
      link = null;
      await welcomeBack(steps, 4, file ? null : { cmp: entry.ver });
    } catch (e) {
      steps.stop();
      throw e;
    } finally {
      P.expecting = false;
      busy(false);
    }
  }

  // --- BOOTSEL --------------------------------------------------------------------

  async function getUf2() {
    const R = await load(false);
    const b = board();
    const entry = manifestFor(R.manifest, b);
    if (!entry) throw new RpcError(`The latest release has no image for the ${b} board.`);
    if (P.uf2 && P.uf2.board === b && P.uf2.ver === entry.ver) return P.uf2;
    const want = uf2Expect(R.history, b, buildOf(entry.ver));
    const steps = stepList(el, $("#fw-steps"), ["Download the .uf2"]);
    steps.start(0);
    try {
      const got = await fetchBytes(imageUrl(base, b, "uf2"), {
        sha256: want && want.sha256, size: want && want.size,
        progress: (a, t) => steps.start(0, `${human(a)} of ${human(t)}`),
      });
      const chip = uf2Chip(got.data);
      if (chip && CHIP[b] && chip !== CHIP[b]) throw new RpcError(`The release's ${b} .uf2 is built for the ${chip}.`);
      steps.done(0, `${human(got.data.length)}${got.verified ? ", SHA-256 matches the release" : ", unverified: the release notes list no checksum for it"}.`);
      P.uf2 = { board: b, ver: entry.ver, data: got.data, name: `rpcortex-v2-${b}.uf2` };
      return P.uf2;
    } catch (e) {
      steps.fail(0, explain(e));
      throw e;
    }
  }

  async function saveUf2() {
    busy(true);
    try {
      const u = await getUf2();
      ctx.saveFile(u.data, u.name);
      ctx.toast(`Saved ${u.name}. Reset the board to BOOTSEL and drag it onto the drive.`);
    } finally { busy(false); }
  }

  async function writeToDrive() {
    busy(true);
    try {
      const u = await getUf2();
      if (ctx.connected()) {
        const go = await ask({
          title: "Reset to BOOTSEL first?",
          body: "The board restarts into its bootloader and a drive named RP2350 or RPI-RP2 appears. Files on the device are kept. Then choose that drive.",
          ok: "Reset to BOOTSEL", danger: true,
        });
        if (!go) return;
        await ctx.bootsel();
        await new Promise((r) => setTimeout(r, 2500));
      }
      // A second click: the folder picker needs a fresh user gesture.
      const pick = await ask({
        title: "Choose the BOOTSEL drive",
        body: `Look for a drive named ${CHIP[u.board] === "RP2040" ? "RPI-RP2" : "RP2350"}. If it is not there, hold the BOOTSEL button while plugging the board in.`,
        ok: "Choose the drive",
      });
      if (!pick) return;
      let dir;
      try {
        dir = await globalThis.showDirectoryPicker({ id: "rpc-bootsel", mode: "readwrite" });
      } catch (e) {
        if (e && e.name === "AbortError") return;
        throw new RpcError(`The browser would not open that drive (${e.message || e.name}). Save the .uf2 and drag it there instead.`);
      }
      const steps = stepList(el, $("#fw-steps"), ["Write the .uf2", "Back again"]);
      steps.start(0, `to ${dir.name}`);
      const r = await writeUf2(dir, u.data, { chip: CHIP[u.board] });
      steps.done(0, r.vanished ? "Written, and the drive went away as the board restarted." : "Written. The board restarts when it has the last block.");
      if (ctx.knownPort()) await welcomeBack(steps, 1, null);
      else steps.done(1, "Connect to it once it has restarted.");
    } finally { busy(false); }
  }

  // --- wiring ------------------------------------------------------------------------

  const run = (fn) => () => { if (!P.busy) fn().catch(error); };
  $("#fw-board").addEventListener("change", () => { P.uf2 = null; renderStatus(); });
  $("#fw-refresh").addEventListener("click", run(async () => { await load(true); renderStatus(); renderNotes(); renderBoards(); }));
  $("#fw-ota").addEventListener("click", run(() => overTheAir(false)));
  $("#fw-send").addEventListener("click", run(() => fromPage(null)));
  $("#fw-send-file").addEventListener("click", () => { if (!P.busy) $("#fw-bin").click(); });
  $("#fw-bin").addEventListener("change", (e) => { const f = e.target.files[0]; e.target.value = ""; if (f) run(() => fromPage(f))(); });
  $("#fw-uf2").addEventListener("click", run(saveUf2));
  $("#fw-uf2-write").addEventListener("click", run(writeToDrive));

  return {
    show,
    refresh: renderStatus,
    // An update in flight expects the port to drop; the page should not call
    // that a surprise.
    expectingRestart: () => P.expecting,
  };
}
