// The package repository, read in the browser: the same index the device's
// `pkg` reads (os/shell/pkgrepo.cpp), the same rule for which architecture
// runs where, and the same check before anything is installed.

import { sha256hex, DigestError, RpcError } from "./link.js";

export const REPO_DEFAULT = "https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo-v2/index.json";

const BOARD_ARCH = { pico2_w: "armv8m", pico2: "armv8m", pico_w: "armv6m", pico: "armv6m" };

export function boardArch(board) {
  if (!board) return null;
  const b = board.toLowerCase();
  if (BOARD_ARCH[b]) return BOARD_ARCH[b];
  if (b.includes("rp2350") || b.startsWith("pico2")) return "armv8m";
  if (b.includes("rp2040") || b.startsWith("pico")) return "armv6m";
  return null;
}

// arch_runs_here() in pkgrepo.cpp: a comma-separated list, unstated means
// allowed, and ARMv8-M Baseline runs ARMv6-M code but not the reverse.
export function archRunsHere(arch, mine) {
  if (!arch || !arch.trim()) return true;
  return arch.split(",").map((a) => a.trim()).filter(Boolean)
    .some((a) => a === mine || (mine === "armv8m" && a === "armv6m"));
}

export function parseIndex(doc, base) {
  const list = doc && Array.isArray(doc.packages) ? doc.packages : null;
  if (!list) throw new RpcError("that is not a package index (no package list)");
  return list.filter((p) => p && p.name && p.url).map((p) => ({
    name: String(p.name), ver: String(p.ver || p.version || ""), desc: String(p.desc || ""),
    url: base ? new URL(p.url, base).href : String(p.url),
    sha256: String(p.sha256 || "").toLowerCase(), arch: String(p.arch || ""),
    kind: String(p.kind || ""), size: Number(p.size || 0), abi: String(p.abi || ""),
  }));
}

export async function loadIndex(url = REPO_DEFAULT, fetchFn = globalThis.fetch) {
  let r;
  try {
    r = await fetchFn(url, { cache: "no-store" });
  } catch (e) {
    throw new RpcError(`could not fetch the package index: ${e.message}`);
  }
  if (!r.ok) throw new RpcError(`the package index answered HTTP ${r.status}`);
  return parseIndex(await r.json(), url);
}

// Names match case-insensitively, as on the device. An entry built for this
// architecture is preferred, then any entry that runs here.
export function choose(packages, name, arch) {
  const same = packages.filter((p) => p.name.toLowerCase() === name.toLowerCase());
  if (!same.length) throw new RpcError(`No package called '${name}'.`);
  if (!arch) return same[0];
  const exact = same.find((p) => p.arch.split(",").map((a) => a.trim()).includes(arch));
  if (exact) return exact;
  const runs = same.find((p) => archRunsHere(p.arch, arch));
  if (runs) return runs;
  throw new RpcError(`${same[0].name} is built for ${same[0].arch}; this board is ${arch}.`);
}

// The package's bytes, refused unless they match the index. Resolves to
// { data, verified }.
export async function download(pkg, fetchFn = globalThis.fetch) {
  const r = await fetchFn(pkg.url, { cache: "no-store" });
  if (!r.ok) throw new RpcError(`${pkg.name}: HTTP ${r.status}`);
  const data = new Uint8Array(await r.arrayBuffer());
  if (!pkg.sha256) return { data, verified: false };
  const have = await sha256hex(data);
  if (have !== pkg.sha256) {
    throw new DigestError(`Checksum mismatch: the download does not match the index (expected ${pkg.sha256}, received ${have}). Nothing was installed.`);
  }
  return { data, verified: true };
}
