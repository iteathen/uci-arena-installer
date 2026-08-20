import { spawnSync } from "node:child_process";
import { lstatSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const SUPPORTED_VERSION = "3.0.6";
const DEFAULT_TIMEOUT_MS = 60_000;
const DEFAULT_MAX_OUTPUT_BYTES = 1024 * 1024;
const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

function regularFile(candidate, label) {
  const resolved = path.resolve(candidate);
  let status;
  try { status = lstatSync(resolved); } catch { throw new Error(`${label} is unavailable`); }
  if (!status.isFile() || status.isSymbolicLink()) throw new Error(`${label} must be a regular file`);
  return resolved;
}

export function resolveCosignExecutable({
  override,
  platform = process.platform,
  arch = process.arch,
  env = process.env,
} = {}) {
  void arch;
  const explicit = override ?? env.COSIGN_EXECUTABLE;
  if (explicit) return regularFile(explicit, "Cosign override");
  const vendor = path.join(repositoryRoot, "build", "tooling", `cosign-${SUPPORTED_VERSION}`, platform === "win32" ? "cosign.exe" : "cosign");
  try { return regularFile(vendor, "vendored Cosign"); } catch { return "cosign"; }
}

function bounded(value, maximum) {
  const text = typeof value === "string" ? value : "";
  return Buffer.byteLength(text, "utf8") <= maximum ? text : `${text.slice(0, maximum)}\n[truncated]`;
}

function invoke(cosignPath, args, env, timeoutMs, maxOutputBytes) {
  const result = spawnSync(cosignPath, args, { encoding: "utf8", env, shell: false, timeout: timeoutMs, maxBuffer: maxOutputBytes, windowsHide: true });
  const stdout = bounded(result.stdout, maxOutputBytes);
  const stderr = bounded(result.stderr, maxOutputBytes);
  if (result.error || result.status !== 0) throw new Error(`Cosign command failed (${result.error?.code ?? result.status ?? "unknown"})`);
  return Object.freeze({ status: result.status, stdout, stderr });
}

export function runCosign({ cosignPath, args, env = process.env, timeoutMs = DEFAULT_TIMEOUT_MS, maxOutputBytes = DEFAULT_MAX_OUTPUT_BYTES }) {
  if (!Array.isArray(args) || args.some((arg) => typeof arg !== "string" || /[\u0000\r\n]/u.test(arg))) throw new Error("Cosign arguments are invalid");
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 300_000 || !Number.isSafeInteger(maxOutputBytes) || maxOutputBytes < 1 || maxOutputBytes > 16 * 1024 * 1024) throw new Error("Cosign execution bounds are invalid");
  if (args[0] !== "version") {
    const version = invoke(cosignPath, ["version", "--json"], env, timeoutMs, maxOutputBytes);
    let parsed;
    try { parsed = JSON.parse(version.stdout); } catch { throw new Error("Cosign version output is invalid"); }
    const semantic = parsed?.gitVersion ?? parsed?.version;
    if (semantic !== `v${SUPPORTED_VERSION}` && semantic !== SUPPORTED_VERSION) throw new Error("unsupported Cosign version");
  }
  return invoke(cosignPath, args, env, timeoutMs, maxOutputBytes);
}
