import { createHash } from "node:crypto";
import {
  lstatSync,
  readFileSync,
  readdirSync,
  statSync,
} from "node:fs";
import path from "node:path";

const SHA256_PATTERN = /^[0-9a-f]{64}$/u;

export function normalizeComponentPath(value, label = "component path") {
  if (typeof value !== "string" || value.length === 0 || value.includes("\\")) {
    throw new Error(`${label} must be a nonempty slash-separated relative path`);
  }
  const normalized = path.posix.normalize(value);
  if (normalized !== value || normalized === "." || normalized.startsWith("../")
      || path.posix.isAbsolute(normalized)) {
    throw new Error(`${label} is unsafe: ${value}`);
  }
  return normalized;
}

export function enumerateComponentFiles(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? `${relative}/${name}` : name;
    const absolute = path.join(root, ...child.split("/"));
    const status = lstatSync(absolute);
    if (status.isSymbolicLink()) {
      throw new Error(`symbolic links are forbidden in component artifacts: ${child}`);
    }
    if (status.isDirectory()) rows.push(...enumerateComponentFiles(root, child));
    else if (status.isFile()) rows.push(child);
    else throw new Error(`unsupported component artifact entry: ${child}`);
  }
  return rows;
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

export function validateExtractedComponent({
  root,
  componentId,
  version,
  target,
}) {
  const resolvedRoot = path.resolve(root);
  if (!lstatSync(resolvedRoot).isDirectory()) {
    throw new Error(`component artifact root is unavailable: ${resolvedRoot}`);
  }
  const manifestPath = path.join(resolvedRoot, "arena-component.json");
  const manifest = readJson(manifestPath, "component manifest");
  if (manifest?.schema !== "arena_install_component_v1"
      || manifest.schema_version !== 1
      || manifest.component_id !== componentId
      || manifest.component_version !== version
      || manifest.target_triple !== target
      || !Array.isArray(manifest.files)
      || !manifest.entrypoints || typeof manifest.entrypoints !== "object") {
    throw new Error("component manifest does not match the requested release identity");
  }

  const declared = new Map();
  for (const file of manifest.files) {
    const relative = normalizeComponentPath(file?.path, "component manifest file path");
    if (declared.has(relative) || !SHA256_PATTERN.test(file?.sha256 ?? "")
        || !Number.isSafeInteger(file?.size_bytes) || file.size_bytes < 0) {
      throw new Error(`invalid or duplicate component manifest file: ${relative}`);
    }
    const absolute = path.join(resolvedRoot, ...relative.split("/"));
    if (!lstatSync(absolute).isFile() || statSync(absolute).size !== file.size_bytes
        || sha256File(absolute) !== file.sha256) {
      throw new Error(`component artifact file identity mismatch: ${relative}`);
    }
    declared.set(relative, absolute);
  }

  const actual = enumerateComponentFiles(resolvedRoot);
  const allowed = new Set([...declared.keys(), "arena-component.json"]);
  const unexpected = actual.filter((relative) => !allowed.has(relative));
  const missing = [...allowed].filter((relative) => !actual.includes(relative));
  if (unexpected.length > 0 || missing.length > 0) {
    throw new Error(
      `component artifact inventory mismatch; unexpected=[${unexpected.join(",")}], missing=[${missing.join(",")}]`,
    );
  }

  for (const [name, value] of Object.entries(manifest.entrypoints)) {
    const relative = normalizeComponentPath(value, `component ${name} entrypoint`);
    if (!declared.has(relative)) {
      throw new Error(`component ${name} entrypoint is not manifest-owned: ${relative}`);
    }
  }
  return { root: resolvedRoot, manifest, declared, manifestPath };
}
