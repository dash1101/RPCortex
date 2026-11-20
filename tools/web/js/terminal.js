// The terminal: xterm.js from a pinned CDN copy with integrity hashes, and a
// plain one that needs nothing if that cannot load (offline, or blocked).

const CDN = "https://cdn.jsdelivr.net/npm/";
const XTERM = {
  js: [CDN + "@xterm/xterm@5.5.0/lib/xterm.js", "sha384-M169f14mRZOXm3hD/v2Ti0ThIT/RnAQagXA9nlE15yHAtrW19gdePJh/HaTzUOe/"],
  css: [CDN + "@xterm/xterm@5.5.0/css/xterm.css", "sha384-8Xk9wy/gzEDUKrXtrmCFa2bBuK3BpjpDuL/p0SeKQX19Khl/M+lHOgD/CyYf7efP"],
  fit: [CDN + "@xterm/addon-fit@0.10.0/lib/addon-fit.js", "sha384-iF+jqbuti4XlB64clWgFWYEscb+UnSRv3VgVikGYZu+otNFnSHr7y7NcKfBnGizn"],
};

const enc = new TextEncoder();

function load(tag, url, integrity) {
  return new Promise((resolve, reject) => {
    const el = document.createElement(tag);
    if (tag === "script") el.src = url;
    else { el.rel = "stylesheet"; el.href = url; }
    el.integrity = integrity;
    el.crossOrigin = "anonymous";
    el.onload = resolve;
    el.onerror = () => reject(new Error("could not load " + url));
    document.head.appendChild(el);
  });
}

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function theme() {
  return {
    background: cssVar("--term-bg"), foreground: cssVar("--term-fg"),
    cursor: cssVar("--accent"), selectionBackground: cssVar("--term-sel"),
  };
}

async function xterm(container, onKeys) {
  await Promise.all([load("script", ...XTERM.js), load("link", ...XTERM.css)]);
  let fit = null;
  try { await load("script", ...XTERM.fit); } catch (e) { /* fixed size then */ }
  const T = globalThis.Terminal;
  if (!T) throw new Error("xterm.js did not define Terminal");
  const term = new T({
    cursorBlink: true, fontSize: 14, scrollback: 5000, theme: theme(),
    fontFamily: cssVar("--mono") || "monospace",
  });
  if (globalThis.FitAddon) {
    fit = new globalThis.FitAddon.FitAddon();
    term.loadAddon(fit);
  }
  term.open(container);
  term.onData((s) => onKeys(enc.encode(s)));
  const refit = () => { try { if (fit && container.offsetParent) fit.fit(); } catch (e) { /* hidden */ } };
  new ResizeObserver(refit).observe(container);
  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => { term.options.theme = theme(); });
  return {
    kind: "xterm",
    write: (bytes) => term.write(bytes),
    clear: () => term.clear(),
    focus: () => term.focus(),
    fit: refit,
  };
}

const KEYS = {
  Enter: "\r", Backspace: "\x7f", Tab: "\t", Escape: "\x1b",
  ArrowUp: "\x1b[A", ArrowDown: "\x1b[B", ArrowRight: "\x1b[C", ArrowLeft: "\x1b[D",
  Home: "\x1b[H", End: "\x1b[F", Delete: "\x1b[3~", PageUp: "\x1b[5~", PageDown: "\x1b[6~",
};

// Enough of a terminal for a shell: text, carriage returns and backspaces,
// colour dropped.
function plain(container, onKeys) {
  const pre = document.createElement("pre");
  pre.className = "plain-term";
  pre.tabIndex = 0;
  pre.setAttribute("aria-label", "Device console");
  container.appendChild(pre);
  const dec = new TextDecoder();
  const lines = [""];
  let col = 0;
  let esc = "";
  const render = () => {
    pre.textContent = lines.join("\n");
    pre.scrollTop = pre.scrollHeight;
  };
  pre.addEventListener("keydown", (e) => {
    let s = null;
    if (e.ctrlKey && !e.altKey && e.key.length === 1) {
      const c = e.key.toUpperCase().charCodeAt(0);
      if (c >= 64 && c <= 95) s = String.fromCharCode(c - 64);
    } else if (KEYS[e.key]) s = KEYS[e.key];
    else if (e.key.length === 1 && !e.metaKey) s = e.key;
    if (s !== null) { e.preventDefault(); onKeys(enc.encode(s)); }
  });
  pre.addEventListener("paste", (e) => {
    e.preventDefault();
    onKeys(enc.encode(e.clipboardData.getData("text").replace(/\r?\n/g, "\r")));
  });
  return {
    kind: "plain",
    write(bytes) {
      for (const ch of dec.decode(bytes, { stream: true })) {
        if (esc) {
          esc += ch;
          if (esc.length > 1 && /[@-~]/.test(ch) && !(esc.length === 2 && ch === "[")) esc = "";
          continue;
        }
        if (ch === "\x1b") { esc = ch; continue; }
        let cur = lines[lines.length - 1];
        if (ch === "\n") { lines.push(""); col = 0; if (lines.length > 3000) lines.splice(0, 500); continue; }
        if (ch === "\r") { col = 0; continue; }
        if (ch === "\b") { col = Math.max(0, col - 1); continue; }
        if (ch < " " && ch !== "\t") continue;
        cur = cur.padEnd(col) ;
        lines[lines.length - 1] = cur.slice(0, col) + ch + cur.slice(col + 1);
        col++;
      }
      render();
    },
    clear() { lines.length = 0; lines.push(""); col = 0; render(); },
    focus() { pre.focus(); },
    fit() {},
  };
}

export async function createTerminal(container, onKeys) {
  try {
    return await xterm(container, onKeys);
  } catch (e) {
    container.textContent = "";
    const t = plain(container, onKeys);
    t.reason = e.message;
    return t;
  }
}
