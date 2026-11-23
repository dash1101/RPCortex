// Packages: what the device has, against the v2 repository index, with the
// device's own version comparison (`pkg upgrade`'s), and installs, upgrades
// and removals with sudo.
//
// Everything is downloaded here, checked against the index's SHA-256 and sent
// to the device, so a board with no Wi-Fi is kept up to date the same way.
// The top half has no DOM and runs in the tests (test/pkgs.test.js); the pane
// at the bottom is the page's.

import { installPackage, quote, stripAnsi, RpcError } from "./link.js";
import { REPO_DEFAULT, archRunsHere, boardArch, choose, download, loadIndex } from "./repo.js";
import { versionCmp } from "./version.js";

// What the board runs: HELLO says (from v2.01-beta), or the board's name.
export function linkArch(hello) {
  const said = hello ? hello.arch : null;
  return said === "armv6m" || said === "armv8m" ? said : boardArch(hello ? hello.board : null);
}

// `_pkgs` is the machine-readable list (pkg.cpp): PKGS_BEGIN, OS:<ver>:<board>,
// PKG:<name>:<ver>, PKGS_END. `pkg list` is the person's, read only when the
// firmware has no `_pkgs`.
export function parseInstalled(text) {
  const t = stripAnsi(text || "");
  const out = { os: null, board: null, list: [] };
  if (t.includes("PKGS_BEGIN")) {
    for (const raw of t.split(/\r?\n/)) {
      const line = raw.trim();
      if (line.startsWith("OS:")) { const p = line.split(":"); out.os = p[1] || null; out.board = p[2] || null; }
      else if (line.startsWith("PKG:")) {
        const p = line.split(":");
        if (p[1]) out.list.push({ name: p[1], ver: p.slice(2).join(":") });
      }
    }
    return out;
  }
  for (const raw of t.split(/\r?\n/)) {
    const m = /^\s{2,}(\S+)\s+(\S+)\s*$/.exec(raw);
    if (m) out.list.push({ name: m[1], ver: m[2] });
  }
  return out;
}

export async function readInstalled(link) {
  const r = await link.exec("_pkgs");
  if (r.rc === 0 && r.text.includes("PKGS_BEGIN")) return parseInstalled(r.text);
  return parseInstalled((await link.exec("pkg list")).text);
}

// The index entry this board would install for `name`, or null. Names match
// case-insensitively, as on the device.
function entryFor(index, name, arch) {
  try { return choose(index, name, arch); } catch (e) { return null; }
}

// One row per installed package, and the rest of the index after them:
//
//   upgrade      the index has a newer version this board can run
//   current      the same version
//   newer        the device has a newer one than the index (a build of its own)
//   elsewhere    the index has it, but not for this board's architecture
//   unlisted     the index does not have it (a built-in package, or another repo)
//   available    not installed, and can be
//   notforboard  not installed, and built for another architecture
export function plan(installed, index, arch) {
  const idx = index || [];
  const have = new Map();
  const rows = [];
  for (const p of installed || []) {
    have.set(p.name.toLowerCase(), true);
    const entry = arch ? entryFor(idx, p.name, arch) : null;
    const any = idx.find((e) => e.name.toLowerCase() === p.name.toLowerCase()) || null;
    let state;
    if (!any) state = "unlisted";
    else if (!entry) state = "elsewhere";
    else {
      const c = versionCmp(entry.ver, p.ver);
      state = c > 0 ? "upgrade" : c === 0 ? "current" : "newer";
    }
    rows.push({ name: p.name, installed: p.ver, entry: entry || any, state });
  }
  const seen = new Set();
  const available = [];
  for (const e of idx) {
    const k = e.name.toLowerCase();
    if (have.has(k) || seen.has(k)) continue;
    seen.add(k);
    const entry = arch ? entryFor(idx, e.name, arch) : null;
    available.push({ name: e.name, installed: null, entry: entry || e, state: entry ? "available" : "notforboard" });
  }
  rows.sort((a, b) => a.name.localeCompare(b.name));
  available.sort((a, b) => a.name.localeCompare(b.name));
  return { rows, available };
}

// Download one package, check it against the index, and install it. Resolves
// to { rc, text, verified }; rc is the device's.
export async function installEntry(link, entry, { fetchFn, progress, signal } = {}) {
  const { data, verified } = await download(entry, fetchFn);
  const r = await installPackage(link, data, entry.name + ".app", { progress, signal });
  return { rc: r.rc, text: r.text, verified };
}

// Every row in `rows` that can be upgraded, one after another. sudo is asked
// for first, so a long run does not stop half way for a password, and the
// link keeps it if the device's five minutes run out. A package that fails
// is reported and the rest still go. `onStep(row, phase, detail)` follows it:
// phase is "start", "done", "failed" or "skipped".
export async function upgradeAll(link, rows, { fetchFn, onStep = () => {}, signal } = {}) {
  const todo = rows.filter((r) => r.state === "upgrade");
  if (!todo.length) return { done: [], failed: [] };
  if (link.hasSudo) await link.authenticate();
  const done = [], failed = [];
  for (const row of todo) {
    if (signal && signal.aborted) { onStep(row, "skipped", "stopped"); continue; }
    onStep(row, "start", `${row.installed} to ${row.entry.ver}`);
    try {
      const r = await installEntry(link, row.entry, { fetchFn, signal });
      if (r.rc === 0) { done.push(row.name); onStep(row, "done", r.text.trim()); }
      else { failed.push(row.name); onStep(row, "failed", r.text.trim()); }
    } catch (e) {
      failed.push(row.name);
      onStep(row, "failed", e.message || String(e));
    }
  }
  return { done, failed };
}

export function removeLine(link, name) { return link.sudoLine("pkg remove " + quote(name)); }

export async function removePackage(link, name) {
  const r = await link.exec(removeLine(link, name));
  if (r.rc !== 0) throw new RpcError(r.text.trim() || `pkg remove ${name} failed`);
  return r;
}

// --- the pane -----------------------------------------------------------------------

const STATE_TEXT = {
  upgrade: "Update available", current: "Up to date", newer: "Newer than the index",
  elsewhere: "Not built for this board", unlisted: "Not in the index",
  available: "", notforboard: "",
};

const ARCH_TEXT = { armv8m: "RP2350 only", armv6m: "any board" };

export function packagesPane(ctx) {
  const { el, ask, explain } = ctx;
  const $ = (s) => document.querySelector(s);
  const P = { index: null, url: null, installed: null, busy: false, loading: null };
  const error = (e) => ctx.showError("packages", e);
  const hello = () => (ctx.link() ? ctx.link().hello : null);

  function log(text) {
    const box = $("#pkg-log");
    box.hidden = false;
    box.textContent += text.replace(/\s+$/, "") + "\n";
    box.scrollTop = box.scrollHeight;
  }

  function busy(on) {
    P.busy = on;
    for (const b of document.querySelectorAll("#pane-packages [data-pk]")) b.disabled = on || b.dataset.off === "1";
  }

  async function loadRepo(force) {
    const url = $("#repo-url").value.trim() || REPO_DEFAULT;
    if (P.index && P.url === url && !force) return P.index;
    if (!P.loading) {
      $("#repo-state").textContent = "Loading the index...";
      P.loading = loadIndex(url).then((idx) => { P.index = idx; P.url = url; $("#repo-state").textContent = `${idx.length} packages in the index.`; return idx; },
        (e) => { $("#repo-state").textContent = `The index could not be read: ${explain(e)}`; throw e; })
        .finally(() => { P.loading = null; });
    }
    return P.loading;
  }

  async function readDevice() {
    const link = await ctx.needLink();
    P.installed = await readInstalled(link);
    return P.installed;
  }

  function archCell(e) {
    if (!e) return el("td", { class: "mono" }, "");
    const a = e.arch || "any";
    return el("td", { class: "mono", title: ARCH_TEXT[e.arch] || "" }, a, e.abi ? el("span", { class: "sub" }, " ABI " + e.abi) : null);
  }

  function render() {
    const arch = linkArch(hello());
    const p = plan(P.installed ? P.installed.list : [], P.index, arch);
    const upgrades = p.rows.filter((r) => r.state === "upgrade");
    const all = $("#pkg-upgrade-all");
    all.dataset.off = upgrades.length ? "0" : "1";
    all.textContent = upgrades.length ? `Update all (${upgrades.length})` : "Update all";
    $("#pkg-summary").textContent = !P.installed ? (ctx.connected() ? "Reading the device..." : "Connect a device to see what it has.")
      : !P.index ? `${p.rows.length} installed. Load the index to compare them.`
      : upgrades.length ? `${upgrades.length} of ${p.rows.length} can be updated.` : `${p.rows.length} installed, all up to date.`;
    $("#pkg-arch").textContent = arch ? `This board runs ${arch} packages${arch === "armv8m" ? " (and armv6m ones)" : ""}.` : hello() ? "This page does not know what this board runs, so it offers nothing to install." : "";

    const rows = $("#pkg-rows");
    rows.textContent = "";
    if (!p.rows.length) rows.append(el("tr", {}, el("td", { colspan: "5", class: "empty" }, P.installed ? "Nothing installed." : "")));
    for (const r of p.rows) {
      const e = r.entry;
      rows.append(el("tr", { class: "state-" + r.state },
        el("td", {}, el("span", { class: "name" }, r.name)),
        el("td", { class: "mono" }, r.installed, r.state === "upgrade" ? el("span", { class: "arrow" }, ` to ${e.ver}`) : null,
          el("div", { class: "sub" }, P.index ? STATE_TEXT[r.state] : "")),
        archCell(e),
        el("td", { class: "wide" }, e ? e.desc : ""),
        el("td", { class: "acts" },
          r.state === "upgrade" ? el("button", { class: "primary", "data-pk": "", onclick: () => go(() => installOne(r)) }, "Update") : null,
          " ",
          el("button", { class: "danger", "data-pk": "", onclick: () => go(() => remove(r.name)), title: `Remove ${r.name}` }, "Remove"))));
    }

    const avail = $("#repo-rows");
    avail.textContent = "";
    const q = $("#repo-filter").value.trim().toLowerCase();
    const shown = p.available.filter((r) => !q || (r.name + " " + (r.entry.desc || "")).toLowerCase().includes(q));
    if (!P.index) avail.append(el("tr", {}, el("td", { colspan: "5", class: "empty" }, $("#repo-state").textContent || "Load the index to see what can be installed.")));
    else if (!shown.length) avail.append(el("tr", {}, el("td", { colspan: "5", class: "empty" }, q ? "Nothing matches." : "Everything in the index is installed.")));
    for (const r of shown) {
      const e = r.entry;
      const runs = r.state === "available" && !!arch && archRunsHere(e.arch, arch);
      avail.append(el("tr", {},
        el("td", {}, el("span", { class: "name" }, r.name), e.kind && e.kind !== "app" ? el("span", { class: "tag" }, e.kind) : null),
        el("td", { class: "mono" }, e.ver),
        archCell(e),
        el("td", { class: "wide" }, e.desc),
        el("td", { class: "acts" }, el("button", {
          "data-pk": "", "data-off": runs ? "0" : "1", disabled: !runs || P.busy,
          title: runs ? `Install ${r.name} ${e.ver}` : arch ? `Built for ${e.arch}; this board is ${arch}` : "Connect first, so the board's architecture is known",
          onclick: () => go(() => installOne(r)),
        }, runs ? "Install" : arch ? (e.arch === "armv8m" ? "Needs an RP2350" : "Not for this board") : "Connect first"))));
    }
    busy(P.busy);
  }

  function go(fn) { if (!P.busy) fn().catch(error).finally(() => busy(false)); }

  async function refresh() {
    try { await loadRepo(false); } catch (e) { /* shown in place */ }
    if (ctx.connected()) await readDevice();
    render();
  }

  async function installOne(row) {
    busy(true);
    const link = await ctx.needLink();
    log(`${row.name} ${row.entry.ver} (${row.entry.arch || "any architecture"}), downloading...`);
    const t = ctx.transferRow(`${row.name} ${row.entry.ver}`, "#pkg-transfers");
    try {
      const r = await installEntry(link, row.entry, { progress: t.progress, signal: t.signal });
      log(r.verified ? "Checksum matches the index." : "The index publishes no checksum for this package; it was installed unverified.");
      log(r.text.trim());
      t.finish(r.rc === 0 ? "installed" : "refused", r.rc !== 0);
    } catch (e) {
      t.finish(t.signal.aborted ? "cancelled" : explain(e), true);
      throw e;
    }
    await readDevice();
    render();
  }

  async function upgradeEvery() {
    busy(true);
    const link = await ctx.needLink();
    await loadRepo(false);
    await readDevice();
    const p = plan(P.installed.list, P.index, linkArch(link.hello));
    const n = p.rows.filter((r) => r.state === "upgrade").length;
    if (!n) { render(); return; }
    const r = await upgradeAll(link, p.rows, {
      onStep: (row, phase, detail) => {
        if (phase === "start") log(`${row.name}: ${detail}...`);
        else log(`${row.name}: ${phase === "done" ? "" : phase + ". "}${detail}`);
      },
    });
    log(r.failed.length ? `Updated ${r.done.length}; ${r.failed.length} did not: ${r.failed.join(", ")}.` : `Updated ${r.done.length}.`);
    await readDevice();
    render();
  }

  async function remove(name) {
    const yes = await ask({
      title: `Remove ${name}?`,
      body: `${name} is uninstalled from the device. Its settings and files elsewhere stay where they are.`,
      ok: "Remove", danger: true,
    });
    if (!yes) return;
    busy(true);
    const link = await ctx.needLink();
    const r = await removePackage(link, name);
    log(r.text.trim());
    await readDevice();
    render();
  }

  async function installFile() {
    const f = $("#pkg-file").files[0];
    if (!f) return;
    busy(true);
    const link = await ctx.needLink();
    const t = ctx.transferRow(`${f.name} to /tmp`, "#pkg-transfers");
    try {
      const r = await installPackage(link, new Uint8Array(await f.arrayBuffer()), f.name, { progress: t.progress, signal: t.signal });
      t.finish(r.rc === 0 ? "installed" : "refused", r.rc !== 0);
      log(r.text.trim());
    } catch (e) {
      t.finish(t.signal.aborted ? "cancelled" : explain(e), true);
      throw e;
    }
    $("#pkg-file").value = "";
    $("#btn-pkg-file").dataset.off = "1";
    await readDevice();
    render();
  }

  $("#repo-url").value = REPO_DEFAULT;
  $("#btn-pkg-list").addEventListener("click", () => go(async () => { busy(true); await loadRepo(true).catch(() => {}); if (ctx.connected()) await readDevice(); render(); }));
  $("#btn-repo-load").addEventListener("click", () => go(async () => { busy(true); await loadRepo(true).catch(() => {}); render(); }));
  $("#repo-filter").addEventListener("input", render);
  $("#pkg-upgrade-all").addEventListener("click", () => go(upgradeEvery));
  $("#pkg-file").addEventListener("change", () => { $("#btn-pkg-file").dataset.off = $("#pkg-file").files.length ? "0" : "1"; busy(P.busy); });
  $("#btn-pkg-file").addEventListener("click", () => go(installFile));

  return {
    show: async () => { render(); await refresh(); },
    render,
  };
}
