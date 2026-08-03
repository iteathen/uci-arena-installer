#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  utimesSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

import {
  enumerateComponentFiles,
  normalizeComponentPath,
} from "./component-artifact.mjs";

const MANIFEST_NAME = "arena-installer-runtime.json";
const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/u;
const SHA256_PATTERN = /^[0-9a-f]{64}$/u;

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function parse(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    const key = flag.slice(2);
    if (Object.hasOwn(result, key)) throw new Error(`duplicate --${key}`);
    result[key] = value;
  }
  return result;
}

function readManifest(filename) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`Installer runtime manifest is not valid JSON: ${error.message}`);
  }
}

function assertIndependentRoots(source, output) {
  const sourceRoot = path.resolve(source);
  const outputRoot = path.resolve(output);
  if (outputRoot === path.parse(outputRoot).root) {
    throw new Error("Installer runtime output cannot be a filesystem root");
  }
  const outputFromSource = path.relative(sourceRoot, outputRoot);
  const sourceFromOutput = path.relative(outputRoot, sourceRoot);
  if (outputRoot === sourceRoot
      || (!outputFromSource.startsWith("..") && !path.isAbsolute(outputFromSource))
      || (!sourceFromOutput.startsWith("..") && !path.isAbsolute(sourceFromOutput))) {
    throw new Error("Installer runtime source and output roots must not overlap");
  }
  return { sourceRoot, outputRoot };
}

export function validateInstallerRuntime({ root, version, target }) {
  const resolvedRoot = path.resolve(root);
  if (!lstatSync(resolvedRoot).isDirectory()) {
    throw new Error(`Installer runtime root is unavailable: ${resolvedRoot}`);
  }
  const manifestPath = path.join(resolvedRoot, MANIFEST_NAME);
  const manifest = readManifest(manifestPath);
  if (manifest?.schema !== "arena_installer_runtime_v1"
      || manifest.schema_version !== 1
      || manifest.product_id !== "uci-arena-installer"
      || manifest.runtime_version !== version
      || manifest.target_triple !== target
      || !Array.isArray(manifest.files)
      || typeof manifest.entrypoints?.setup !== "string") {
    throw new Error("Installer runtime does not match the requested identity");
  }

  const declared = new Map();
  for (const row of manifest.files) {
    const relative = normalizeComponentPath(row?.path, "Installer runtime file path");
    if (declared.has(relative) || relative === MANIFEST_NAME
        || !SHA256_PATTERN.test(row?.sha256 ?? "")
        || !Number.isSafeInteger(row?.size_bytes) || row.size_bytes < 1) {
      throw new Error(`invalid Installer runtime file declaration: ${relative}`);
    }
    const absolute = path.join(resolvedRoot, ...relative.split("/"));
    if (!lstatSync(absolute).isFile()
        || statSync(absolute).size !== row.size_bytes
        || sha256File(absolute) !== row.sha256) {
      throw new Error(`Installer runtime file identity mismatch: ${relative}`);
    }
    declared.set(relative, absolute);
  }

  const entrypoint = normalizeComponentPath(
    manifest.entrypoints.setup, "Installer setup entrypoint",
  );
  if (!declared.has(entrypoint)) {
    throw new Error(`Installer setup entrypoint is not manifest-owned: ${entrypoint}`);
  }
  const actual = enumerateComponentFiles(resolvedRoot);
  const allowed = new Set([...declared.keys(), MANIFEST_NAME]);
  const unexpected = actual.filter((relative) => !allowed.has(relative));
  const missing = [...allowed].filter((relative) => !actual.includes(relative));
  if (unexpected.length > 0 || missing.length > 0) {
    throw new Error(
      `Installer runtime inventory mismatch; unexpected=[${unexpected.join(",")}], missing=[${missing.join(",")}]`,
    );
  }
  return { root: resolvedRoot, manifest, manifestPath, declared, entrypoint };
}

export function buildInstallerRuntime({
  installTree,
  outputRoot,
  version,
  target,
  sourceDateEpoch,
}) {
  if (!VERSION_PATTERN.test(version ?? "")
      || !/^(windows|linux)-x86_64$/u.test(target ?? "")) {
    throw new Error("Installer runtime requires an immutable version and supported target");
  }
  const epoch = Number(sourceDateEpoch);
  if (!Number.isSafeInteger(epoch) || epoch < 0) {
    throw new Error("source date epoch must be a nonnegative integer");
  }
  const { sourceRoot, outputRoot: output } = assertIndependentRoots(
    installTree, outputRoot,
  );
  if (!lstatSync(sourceRoot).isDirectory()) {
    throw new Error(`Installer install tree is unavailable: ${sourceRoot}`);
  }
  const sourceFiles = enumerateComponentFiles(sourceRoot);
  if (sourceFiles.includes(MANIFEST_NAME)) {
    throw new Error("Installer install tree must not contain a prebuilt runtime manifest");
  }
  const setup = target.startsWith("windows")
    ? "bin/arena-setup.exe" : "bin/arena-setup";
  if (!sourceFiles.includes(setup)) {
    throw new Error(`Installer install tree is missing ${setup}`);
  }
  if (!sourceFiles.some((relative) =>
    /(^|\/)(license|copying|copyright)(\.|$)/iu.test(relative))) {
    throw new Error("Installer runtime must contain license evidence");
  }

  const staging = `${output}.staging-${process.pid}`;
  const previous = `${output}.previous-${process.pid}`;
  rmSync(staging, { recursive: true, force: true });
  rmSync(previous, { recursive: true, force: true });
  mkdirSync(staging, { recursive: false });
  try {
    for (const relative of sourceFiles) {
      const destination = path.join(staging, ...relative.split("/"));
      mkdirSync(path.dirname(destination), { recursive: true });
      copyFileSync(path.join(sourceRoot, ...relative.split("/")), destination);
      chmodSync(destination, relative.startsWith("bin/") ? 0o755 : 0o644);
      utimesSync(destination, epoch, epoch);
    }
    const rows = enumerateComponentFiles(staging).map((relative) => {
      const filename = path.join(staging, ...relative.split("/"));
      return {
        path: relative,
        size_bytes: statSync(filename).size,
        sha256: sha256File(filename),
      };
    });
    const manifest = {
      schema: "arena_installer_runtime_v1",
      schema_version: 1,
      product_id: "uci-arena-installer",
      runtime_version: version,
      target_triple: target,
      entrypoints: { setup },
      files: rows,
    };
    writeFileSync(
      path.join(staging, MANIFEST_NAME), `${JSON.stringify(manifest, null, 2)}\n`,
    );
    validateInstallerRuntime({ root: staging, version, target });

    const hadPrevious = existsSync(output);
    if (hadPrevious) renameSync(output, previous);
    try {
      renameSync(staging, output);
      const built = validateInstallerRuntime({ root: output, version, target });
      if (hadPrevious) rmSync(previous, { recursive: true, force: true });
      return built;
    } catch (error) {
      let rollbackError;
      try {
        if (existsSync(output)) rmSync(output, { recursive: true, force: true });
        if (hadPrevious && existsSync(previous)) renameSync(previous, output);
      } catch (failure) {
        rollbackError = failure;
      }
      if (rollbackError) {
        throw new AggregateError(
          [error, rollbackError],
          `Installer runtime commit failed and rollback also failed; previous=${previous}`,
        );
      }
      throw error;
    }
  } finally {
    rmSync(staging, { recursive: true, force: true });
  }
}

function main() {
  const args = parse(process.argv.slice(2));
  for (const required of [
    "install-tree", "output-root", "version", "target", "source-date-epoch",
  ]) {
    if (!args[required]) throw new Error(`missing --${required}`);
  }
  const built = buildInstallerRuntime({
    installTree: args["install-tree"],
    outputRoot: args["output-root"],
    version: args.version,
    target: args.target,
    sourceDateEpoch: args["source-date-epoch"],
  });
  process.stdout.write(`${JSON.stringify({
    schema: "arena_installer_runtime_build_v1",
    root: built.root,
    manifest: built.manifestPath,
    files: built.declared.size,
  })}\n`);
}

if (import.meta.url === `file://${process.argv[1]?.replaceAll("\\", "/")}`) {
  main();
}
