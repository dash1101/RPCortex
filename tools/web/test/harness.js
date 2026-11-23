// A few lines of test runner, shared by the test files that came after
// link.test.js. No dependencies: Node 20 or later, or Deno.
//
// Every test has a time limit, so a flow that waits for an answer the mock
// never gives (a yes/no question over EXEC, say) fails instead of hanging.

const tests = [];
export const test = (name, fn, ms = 20000) => tests.push({ name, fn, ms });

export function eq(a, b, what) {
  const x = JSON.stringify(a), y = JSON.stringify(b);
  if (x !== y) throw new Error(`${what || "values differ"}: ${x} != ${y}`);
}
export function ok(v, what) { if (!v) throw new Error(what || "expected true"); }
export async function rejects(p, cls, what) {
  try { await p; } catch (e) {
    if (!cls || e instanceof cls) return e;
    throw new Error(`${what}: wrong error ${e.name}: ${e.message}`);
  }
  throw new Error(`${what || "expected a rejection"}`);
}
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function limit(p, ms) {
  let timer;
  const t = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error(`no result in ${ms} ms`)), ms); });
  return Promise.race([p, t]).finally(() => clearTimeout(timer));
}

export async function run() {
  let failed = 0;
  for (const t of tests) {
    const t0 = Date.now();
    try {
      await limit(Promise.resolve().then(t.fn), t.ms);
      console.log(`  ok    ${t.name}  (${Date.now() - t0} ms)`);
    } catch (e) {
      failed++;
      console.log(`  FAIL  ${t.name}\n        ${e && e.stack ? e.stack.split("\n").slice(0, 3).join("\n        ") : e}`);
    }
  }
  console.log(`\n  ${tests.length - failed} passed, ${failed} failed`);
  const exit = globalThis.process ? (c) => globalThis.process.exit(c) : (c) => globalThis.Deno.exit(c);
  exit(failed ? 1 : 0);
}
