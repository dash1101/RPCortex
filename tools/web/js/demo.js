// Demo mode (index.html?demo): a pretend device behind a pretend Web Serial
// port, so the page can be tried, and tested, with no board attached.
//
// The pretend device runs v2.01-beta, so the Firmware tab shows the case of a
// device that needs --force once, and its update and restart can be watched
// end to end. It restarts as a board does: the port drops, and a moment later
// it can be opened again, running what it was updated to.

import { MockDevice } from "./mockdev.js";
import { RELEASES_DEFAULT } from "./updates.js";

const RESTART_MS = 2500;

// What the pretend device fetches for `update check`: the real latest.json
// when the page can reach it, and a stand-in when it cannot.
const FALLBACK = { packages: [{ name: "pico2_w", ver: "2.0.640", desc: "RPCortex v2.0.640-beta Vela II for pico2_w", size: 868604 }] };

class DemoPort {
  constructor(serial) { this.serial = serial; this.dev = null; this.readable = null; this.writable = null; this.downAt = 0; }

  async open({ baudRate }) {
    if (baudRate === 1200) {          // the BOOTSEL reset: the device goes away
      if (this.dev) this.dev.stop();
      this.dev = null;
      return;
    }
    if (this.dev && this.dev.gone) {
      // Still restarting: the browser cannot open it yet.
      if (Date.now() - this.downAt < RESTART_MS) throw new DOMException("Failed to open serial port.", "NetworkError");
      this.dev.revive();
    }
    if (!this.dev) {
      this.dev = new MockDevice({ noise: 0.03, manifest: FALLBACK, updateDelay: 4000 }).start();
      // What sudo asks for in the demo; the page says so when it asks.
      this.dev.users.root = "demo";
      this.dev.files.set("/home/root/hello.txt", new TextEncoder().encode("Hello from the demo device.\n"));
      const dev = this.dev;
      const base = new URLSearchParams(location.search).get("releases") || RELEASES_DEFAULT;
      fetch(base + "latest.json", { cache: "no-store" }).then((r) => (r.ok ? r.json() : null))
        .then((j) => { if (j && Array.isArray(j.packages)) dev.manifest = j; }).catch(() => {});
    }
    const dev = this.dev;
    let ctl = null;
    dev.onReboot = () => {
      this.downAt = Date.now();
      setTimeout(() => { try { ctl.error(new Error("the device disconnected")); } catch (e) { /* closed */ } }, 50);
      setTimeout(() => this.serial.fire("connect", this), RESTART_MS + 100);
    };
    let cancelled = false;
    const from = dev.toHost, to = dev.fromHost;
    this.readable = new ReadableStream({
      start: (c) => { ctl = c; },
      pull: async (c) => {
        let b;
        try { b = await from.take(65536, null); } catch (e) {
          // The device went away. A real port's stream errors then (the
          // device was lost); it never just ends, and serial.js reads an
          // ended one as "open it again".
          try { c.error(new Error("the device disconnected")); } catch (e2) { /* errored */ }
          return;
        }
        // A read still waiting when the port was closed must not keep what
        // the next opening is owed.
        if (cancelled) from.unshift(b); else c.enqueue(b);
      },
      cancel: () => { cancelled = true; },
    });
    this.writable = new WritableStream({ write: (chunk) => to.push(new Uint8Array(chunk)) });
  }

  async setSignals() {}

  async close() {
    try { await this.readable.cancel(); } catch (e) { /* locked or gone */ }
    this.readable = null;
    this.writable = null;
  }

  getInfo() { return { usbVendorId: 0x2e8a, usbProductId: 0x0011 }; }
}

export function demoSerial() {
  const listeners = {};
  const serial = {
    requestPort: async () => port,
    getPorts: async () => [port],
    addEventListener: (k, f) => { (listeners[k] = listeners[k] || []).push(f); },
    fire: (k, target) => { for (const f of listeners[k] || []) f({ target }); },
  };
  const port = new DemoPort(serial);
  return serial;
}
