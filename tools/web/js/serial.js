// One Web Serial port, shared by the terminal and link mode.
//
// A single reader runs for as long as the port is open. While a sink is set
// (the terminal), bytes go straight to it; otherwise they queue for read(),
// which is the byte stream link.js expects.

import { RpcError } from "./link.js";

export const RPI_VID = 0x2e8a;

export function webSerialSupported() {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

export class SerialLine {
  constructor(port) {
    this.port = port;
    this.queue = [];
    this.waiters = [];
    this.sink = null;
    this.closed = true;
    this.closing = false;
    this.onLost = null;
    this.reader = null;
    this.writer = null;
  }

  async open() {
    await this.port.open({ baudRate: 115200, bufferSize: 65536 });
    // The Pico SDK's USB console counts the host as connected only while DTR
    // is asserted, and drops what it prints otherwise.
    try { await this.port.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) { /* not every driver */ }
    this.writer = this.port.writable.getWriter();
    this.closed = false;
    this.closing = false;
    this._loop();
  }

  async _loop() {
    let error = null;
    try {
      while (this.port.readable && !this.closing) {
        this.reader = this.port.readable.getReader();
        let ended = false;
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) { ended = true; break; }
            if (value && value.length) this._push(value);
          }
        } catch (e) {
          error = e;
          break;
        } finally {
          try { this.reader.releaseLock(); } catch (e) { /* already released */ }
        }
        // A stream that ended is a port that is gone. Asking it for another
        // reader would get `done` straight away, for ever: the page froze
        // when a port closed cleanly instead of erroring.
        if (this.closing || ended) break;
      }
    } finally {
      this.closed = true;
      this._wake();
      if (!this.closing && this.onLost) this.onLost(error);
    }
  }

  _push(bytes) {
    if (this.sink) this.sink(bytes);
    else { this.queue.push(bytes); this._wake(); }
  }

  _wake() {
    const w = this.waiters;
    this.waiters = [];
    for (const f of w) f();
  }

  // The terminal takes bytes as they come; null hands them to read() instead.
  setSink(fn) {
    this.sink = fn;
    if (fn && this.queue.length) {
      const q = this.queue;
      this.queue = [];
      for (const b of q) fn(b);
    }
  }

  discard() { this.queue = []; }

  async read(ms) {
    if (!this.queue.length && !this.closed) {
      await new Promise((resolve) => {
        const done = () => { clearTimeout(timer); resolve(); };
        const timer = setTimeout(() => {
          const i = this.waiters.indexOf(done);
          if (i >= 0) this.waiters.splice(i, 1);
          resolve();
        }, ms);
        this.waiters.push(done);
      });
    }
    if (!this.queue.length) {
      if (this.closed) throw new RpcError("the port closed");
      return new Uint8Array(0);
    }
    const q = this.queue;
    this.queue = [];
    let n = 0;
    for (const b of q) n += b.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const b of q) { out.set(b, o); o += b.length; }
    return out;
  }

  async write(bytes) {
    if (this.closed) throw new RpcError("the port is closed");
    await this.writer.write(bytes);
  }

  async close() {
    this.closing = true;
    try { if (this.reader) await this.reader.cancel(); } catch (e) { /* gone already */ }
    try { if (this.writer) this.writer.releaseLock(); } catch (e) { /* gone already */ }
    try { await this.port.close(); } catch (e) { /* gone already */ }
    this.closed = true;
    this._wake();
  }

  info() {
    return this.port.getInfo ? this.port.getInfo() : {};
  }
}

// Opening the port at 1200 baud makes the Pico SDK reboot the board into
// BOOTSEL. The port must not be open already.
export async function resetToBootsel(port) {
  try { await port.open({ baudRate: 1200 }); } catch (e) { return; }
  try { await port.setSignals({ dataTerminalReady: true }); } catch (e) { /* the reset can win the race */ }
  await new Promise((r) => setTimeout(r, 100));
  try { await port.close(); } catch (e) { /* the board has already gone */ }
}
