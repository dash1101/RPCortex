// Demo mode (index.html?demo): a pretend device behind a pretend Web Serial
// port, so the page can be tried, and tested, with no board attached.

import { MockDevice } from "./mockdev.js";

class DemoPort {
  constructor() { this.dev = null; this.readable = null; this.writable = null; }

  async open({ baudRate }) {
    if (baudRate === 1200) {          // the BOOTSEL reset: the device goes away
      if (this.dev) this.dev.stop();
      this.dev = null;
      return;
    }
    if (!this.dev) {
      this.dev = new MockDevice({ noise: 0.03 }).start();
      this.dev.files.set("/home/root/hello.txt", new TextEncoder().encode("Hello from the demo device.\n"));
    }
    const dev = this.dev;
    let ctl = null;
    dev.onReboot = () => {
      setTimeout(() => { try { ctl.error(new Error("the device disconnected")); } catch (e) { /* closed */ } }, 50);
      this.dev = null;
    };
    let cancelled = false;
    this.readable = new ReadableStream({
      start: (c) => { ctl = c; },
      pull: async (c) => {
        let b;
        try { b = await dev.toHost.take(65536, null); } catch (e) { try { c.close(); } catch (e2) { /* errored */ } return; }
        // A read still waiting when the port was closed must not keep what
        // the next opening is owed.
        if (cancelled) dev.toHost.unshift(b); else c.enqueue(b);
      },
      cancel: () => { cancelled = true; },
    });
    this.writable = new WritableStream({ write: (chunk) => dev.fromHost.push(new Uint8Array(chunk)) });
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
  const port = new DemoPort();
  const listeners = {};
  return {
    requestPort: async () => port,
    getPorts: async () => [port],
    addEventListener: (k, f) => { listeners[k] = f; },
  };
}
