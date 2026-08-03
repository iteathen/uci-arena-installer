import {
  chmodSync,
  copyFileSync,
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
import { gzipSync } from "node:zlib";

import {
  enumerateComponentFiles,
  normalizeComponentPath,
  validateExtractedComponent,
} from "./component-artifact.mjs";

const VERSION_PATTERN = /^\d+\.\d+\.\d+$/u;

function regularFile(filename, label) {
  const resolved = path.resolve(filename);
  let status;
  try {
    status = lstatSync(resolved);
  } catch {
    throw new Error(`${label} is unavailable: ${resolved}`);
  }
  if (!status.isFile() || status.isSymbolicLink()) {
    throw new Error(`${label} must be a regular file: ${resolved}`);
  }
  return resolved;
}

function readPolicy(filename) {
  const policyPath = regularFile(filename, "Linux integration policy");
  let policy;
  try {
    policy = JSON.parse(readFileSync(policyPath, "utf8"));
  } catch (error) {
    throw new Error(`Linux integration policy is not valid JSON: ${error.message}`);
  }
  if (policy?.schema !== "arena_manager_linux_integration_v1"
      || policy.component_id !== "manager.core"
      || !Array.isArray(policy.component_mappings)
      || !Array.isArray(policy.integration_files)
      || !Array.isArray(policy.component_aliases)
      || !policy.package || !policy.documentation || !policy.maintainer_scripts) {
    throw new Error("Linux integration policy identity or structure is invalid");
  }
  const catchAll = policy.component_mappings.filter((row) => row?.source_prefix === "");
  if (catchAll.length !== 1
      || policy.component_mappings.at(-1)?.source_prefix !== "") {
    throw new Error("Linux component mappings require one final catch-all mapping");
  }
  return { policy, policyPath, policyRoot: path.dirname(policyPath) };
}

function destinationForComponent(relative, mappings) {
  for (const mapping of mappings) {
    const sourcePrefix = mapping?.source_prefix;
    if (typeof sourcePrefix !== "string" || typeof mapping.destination_prefix !== "string") {
      throw new Error("Linux component mapping must contain string prefixes");
    }
    if (relative.startsWith(sourcePrefix)) {
      return normalizeComponentPath(
        `${mapping.destination_prefix}${relative.slice(sourcePrefix.length)}`,
        "Linux component destination",
      );
    }
  }
  throw new Error(`Linux policy does not map Manager component path: ${relative}`);
}

function addEntry(entries, destination, source, mode, owner) {
  const normalized = normalizeComponentPath(destination, `${owner} destination`);
  const resolvedSource = regularFile(source, `${owner} source`);
  const normalizedMode = Number(mode);
  if (!Number.isInteger(normalizedMode) || normalizedMode < 0 || normalizedMode > 0o777) {
    throw new Error(`${owner} mode is invalid for ${normalized}`);
  }
  if (entries.has(normalized)) {
    throw new Error(`Linux package destination collision: ${normalized}`);
  }
  entries.set(normalized, { source: resolvedSource, mode: normalizedMode, owner });
}

function assertIndependentRoots(source, output) {
  const sourceRoot = path.resolve(source);
  const outputRoot = path.resolve(output);
  const outputFromSource = path.relative(sourceRoot, outputRoot);
  const sourceFromOutput = path.relative(outputRoot, sourceRoot);
  if (outputRoot === path.parse(outputRoot).root || sourceRoot === outputRoot
      || (!outputFromSource.startsWith("..") && !path.isAbsolute(outputFromSource))
      || (!sourceFromOutput.startsWith("..") && !path.isAbsolute(sourceFromOutput))) {
    throw new Error("Linux package source and staging roots must not overlap");
  }
  return outputRoot;
}

export function createManagerLinuxPackagePlan({ managerRoot, policyPath, version }) {
  if (!VERSION_PATTERN.test(version ?? "")) {
    throw new Error("Linux Manager package requires a three-field version");
  }
  const { policy, policyRoot } = readPolicy(policyPath);
  const manager = validateExtractedComponent({
    root: path.resolve(managerRoot),
    componentId: policy.component_id,
    version,
    target: "linux-x86_64",
  });
  const entries = new Map();
  for (const [relative, source] of manager.declared) {
    const destination = destinationForComponent(relative, policy.component_mappings);
    const executable = relative.startsWith("bin/")
      || (statSync(source).mode & 0o111) !== 0;
    addEntry(entries, destination, source, executable ? 0o755 : 0o644, "Manager component");
  }
  addEntry(
    entries,
    policy.component_manifest_destination,
    manager.manifestPath,
    0o644,
    "Manager component manifest",
  );
  for (const row of policy.integration_files) {
    addEntry(
      entries,
      row?.destination,
      path.join(policyRoot, normalizeComponentPath(row?.source, "integration source")),
      row?.mode,
      "Installer integration",
    );
  }
  for (const row of policy.component_aliases) {
    const componentSource = normalizeComponentPath(row?.source, "component alias source");
    const source = manager.declared.get(componentSource);
    if (!source) throw new Error(`component alias is not manifest-owned: ${componentSource}`);
    addEntry(entries, row?.destination, source, row?.mode, "Installer component alias");
  }
  const manualSource = normalizeComponentPath(
    policy.documentation.manual_source, "manual source",
  );
  if (!manager.declared.has(manualSource)) {
    throw new Error(`manual source is not manifest-owned: ${manualSource}`);
  }
  const generated = {
    manualSource: manager.declared.get(manualSource),
    manualDestination: normalizeComponentPath(
      policy.documentation.manual_destination, "manual destination",
    ),
    changelogSource: regularFile(
      path.join(
        policyRoot,
        normalizeComponentPath(policy.documentation.changelog_source, "changelog source"),
      ),
      "changelog template",
    ),
    changelogDestination: normalizeComponentPath(
      policy.documentation.changelog_destination, "changelog destination",
    ),
  };
  if (generated.manualDestination === generated.changelogDestination
      || entries.has(generated.manualDestination)
      || entries.has(generated.changelogDestination)) {
    throw new Error("Linux generated documentation destination collides with package payload");
  }
  const maintainerScripts = {};
  for (const [format, scripts] of Object.entries(policy.maintainer_scripts)) {
    maintainerScripts[format] = {};
    for (const [name, relative] of Object.entries(scripts)) {
      maintainerScripts[format][name] = regularFile(
        path.join(policyRoot, normalizeComponentPath(relative, "maintainer script")),
        `${format} ${name} script`,
      );
    }
  }
  if (!maintainerScripts.deb?.postinst || !maintainerScripts.deb?.prerm
      || !maintainerScripts.deb?.postrm || !maintainerScripts.rpm?.post
      || !maintainerScripts.rpm?.preun || !maintainerScripts.rpm?.postun) {
    throw new Error("Linux maintainer-script contract is incomplete");
  }
  return { policy, manager, entries, generated, maintainerScripts, version };
}

function writeGeneratedGzip(destination, text, epoch) {
  mkdirSync(path.dirname(destination), { recursive: true });
  writeFileSync(destination, gzipSync(Buffer.from(text), { level: 9, mtime: 0 }));
  chmodSync(destination, 0o644);
  utimesSync(destination, epoch, epoch);
}

export function stageManagerLinuxPackage({
  managerRoot,
  policyPath,
  version,
  outputRoot,
  sourceDateEpoch,
}) {
  const epoch = Number(sourceDateEpoch);
  if (!Number.isSafeInteger(epoch) || epoch < 0) {
    throw new Error("source date epoch must be a nonnegative integer");
  }
  const plan = createManagerLinuxPackagePlan({ managerRoot, policyPath, version });
  const output = assertIndependentRoots(managerRoot, outputRoot);
  const staging = `${output}.staging-${process.pid}`;
  const previous = `${output}.previous-${process.pid}`;
  rmSync(staging, { recursive: true, force: true });
  rmSync(previous, { recursive: true, force: true });
  mkdirSync(staging, { recursive: false });
  try {
    for (const [relative, entry] of plan.entries) {
      const destination = path.join(staging, ...relative.split("/"));
      mkdirSync(path.dirname(destination), { recursive: true });
      copyFileSync(entry.source, destination);
      chmodSync(destination, entry.mode);
      utimesSync(destination, epoch, epoch);
    }
    writeGeneratedGzip(
      path.join(staging, ...plan.generated.manualDestination.split("/")),
      readFileSync(plan.generated.manualSource, "utf8"),
      epoch,
    );
    const changelog = readFileSync(plan.generated.changelogSource, "utf8")
      .replaceAll("@PROJECT_VERSION@", version);
    writeGeneratedGzip(
      path.join(staging, ...plan.generated.changelogDestination.split("/")),
      changelog,
      epoch,
    );
    if (readdirSync(staging).length === 0) throw new Error("Linux package stage is empty");
    const expectedFiles = enumerateComponentFiles(staging);
    const hadPrevious = Boolean(lstatSync(output, { throwIfNoEntry: false }));
    if (hadPrevious) renameSync(output, previous);
    try {
      renameSync(staging, output);
      const observedFiles = enumerateComponentFiles(output);
      if (JSON.stringify(observedFiles) !== JSON.stringify(expectedFiles)) {
        throw new Error("Linux package stage changed while committing");
      }
      if (hadPrevious) rmSync(previous, { recursive: true, force: true });
    } catch (error) {
      let rollbackError;
      try {
        if (lstatSync(output, { throwIfNoEntry: false })) {
          rmSync(output, { recursive: true, force: true });
        }
        if (hadPrevious && lstatSync(previous, { throwIfNoEntry: false })) {
          renameSync(previous, output);
        }
      } catch (failure) {
        rollbackError = failure;
      }
      if (rollbackError) {
        throw new AggregateError(
          [error, rollbackError],
          `Linux package stage commit failed and rollback also failed; previous=${previous}`,
        );
      }
      throw error;
    }
    return { ...plan, root: output, files: enumerateComponentFiles(output) };
  } finally {
    rmSync(staging, { recursive: true, force: true });
  }
}
