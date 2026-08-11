import {
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import {
  sourceIdentity,
  transactionalOutput,
  verifyAndExtractCandidates,
} from "./clean-build.mjs";
import { deterministicTarGz } from "./deterministic-tar.mjs";
import {
  loadReleaseInputs,
  sha256File,
  validateReleaseLock,
} from "./release-lock.mjs";
import { resolveCosignExecutable } from "./provider-provenance.mjs";
import { buildSuiteCatalog } from "./suite-catalog.mjs";

const repositoryRoot = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const supportedPlatform = "windows-x86_64";

function safeRelative(relative) {
  return typeof relative === "string"
    && relative.length > 0
    && !path.isAbsolute(relative)
    && !relative.split(/[\\/]/u).some((part) => part === "" || part === "." || part === "..");
}

function fileRecord(root, filename) {
  const relative = path.relative(root, filename).split(path.sep).join("/");
  if (!safeRelative(relative)) throw new Error(`unsafe update-package path: ${relative}`);
  const status = lstatSync(filename);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`invalid update-package file: ${relative}`);
  }
  return { path: relative, size_bytes: status.size, sha256: sha256File(filename) };
}

function copyMetadataTree(source, destination) {
  const required = ["root.json", "targets.json", "snapshot.json", "timestamp.json"];
  for (const name of required) {
    const filename = path.join(source, name);
    if (!existsSync(filename) || !lstatSync(filename).isFile()
        || lstatSync(filename).isSymbolicLink()) {
      throw new Error(`trusted update metadata is incomplete: ${name}`);
    }
  }
  mkdirSync(destination, { recursive: true });
  for (const entry of readdirSync(source, { withFileTypes: true })) {
    if (!entry.isFile() || entry.isSymbolicLink() || !entry.name.endsWith(".json")) {
      throw new Error(`trusted update metadata contains an unsupported entry: ${entry.name}`);
    }
    copyFileSync(path.join(source, entry.name), path.join(destination, entry.name));
  }
}

function dependencyClosure(composition, selectedComponents) {
  const byId = new Map(composition.components.map((component) => [component.component_id, component]));
  const closure = new Set();
  const visit = (id) => {
    const component = byId.get(id);
    if (!component) throw new Error(`update component is not composed: ${id}`);
    if (component.role === "manager") {
      throw new Error("manager.core changes require the full MSI/installer build path");
    }
    if (closure.has(id)) return;
    closure.add(id);
    for (const dependency of component.requires) visit(dependency);
  };
  for (const id of selectedComponents) visit(id);
  return [...closure].sort();
}

export function assembleSuiteUpdatePackage({
  composition,
  candidates,
  selectedComponents,
  outputRoot,
  sourceCommit,
  sourceEpoch,
  compositionSha256,
  releaseLockSha256,
  inputAuthority,
  metadataRoot = "",
  applyScript = path.join(repositoryRoot, "packaging", "windows", "apply-suite-update.ps1"),
}) {
  if (!/^[0-9a-f]{40}$/u.test(sourceCommit ?? "")) {
    throw new Error("update package requires an exact source commit");
  }
  if (!Number.isSafeInteger(sourceEpoch) || sourceEpoch < 0) {
    throw new Error("update package requires a valid source epoch");
  }
  if (!Array.isArray(selectedComponents) || selectedComponents.length < 1
      || new Set(selectedComponents).size !== selectedComponents.length) {
    throw new Error("update package requires unique selected components");
  }
  const closure = dependencyClosure(composition, selectedComponents);
  const trusted = metadataRoot !== "";
  if ((inputAuthority === "release_ready_lock") !== trusted) {
    throw new Error("release-ready update packages require trusted metadata; diagnostics forbid it");
  }
  if (!existsSync(applyScript) || !lstatSync(applyScript).isFile()) {
    throw new Error("fixed update deployment script is unavailable");
  }

  return transactionalOutput(outputRoot, ({ publishRoot, workRoot }) => {
    const packageRoot = path.join(workRoot, "package");
    const releaseRoot = path.join(packageRoot, "release");
    const payloadRoot = path.join(releaseRoot, "payloads");
    mkdirSync(payloadRoot, { recursive: true });
    const catalog = buildSuiteCatalog({
      composition,
      target: supportedPlatform,
      candidates,
      featureComponentIds: [],
      channel: trusted ? "stable" : "development",
    });
    const catalogPath = path.join(releaseRoot, "catalog.json");
    writeFileSync(catalogPath, `${JSON.stringify(catalog, null, 2)}\n`);
    for (const candidate of candidates) {
      copyFileSync(
        candidate.artifactPath,
        path.join(payloadRoot, candidate.lockRow.artifact_name),
      );
    }
    if (trusted) copyMetadataTree(metadataRoot, path.join(releaseRoot, "tuf"));
    const scriptPath = path.join(packageRoot, "apply-update.ps1");
    copyFileSync(applyScript, scriptPath);

    const files = [];
    const visit = (directory) => {
      for (const entry of readdirSync(directory, { withFileTypes: true })) {
        const filename = path.join(directory, entry.name);
        if (entry.isDirectory()) visit(filename);
        else files.push(fileRecord(packageRoot, filename));
      }
    };
    visit(packageRoot);
    files.sort((left, right) => left.path.localeCompare(right.path));
    const packageManifest = {
      schema: "arena_suite_update_package_v1",
      schema_version: 1,
      suite_version: composition.product_release,
      target_platform: supportedPlatform,
      installer_minimum_version: composition.installer.minimum_version,
      input_authority: inputAuthority,
      trust: trusted ? "tuf" : "unsigned_development",
      selected_components: [...selectedComponents].sort(),
      dependency_closure: closure,
      files,
    };
    const packageManifestPath = path.join(packageRoot, "update-package.json");
    writeFileSync(packageManifestPath, `${JSON.stringify(packageManifest, null, 2)}\n`);

    const archiveName =
      `UCI-Arena-Suite-Update-${composition.product_release}-windows-x64.tar.gz`;
    const archivePath = path.join(publishRoot, archiveName);
    writeFileSync(archivePath, deterministicTarGz(packageRoot, sourceEpoch));
    const result = {
      schema: "arena_suite_update_result_v1",
      schema_version: 1,
      suite_version: composition.product_release,
      source_commit: sourceCommit,
      target_platform: supportedPlatform,
      input_authority: inputAuthority,
      build_path: "package_only_no_native_compilation",
      full_installer_build_required: false,
      composition_sha256: compositionSha256,
      release_lock_sha256: releaseLockSha256,
      selected_components: packageManifest.selected_components,
      dependency_closure: closure,
      package_manifest_sha256: sha256File(packageManifestPath),
      artifact: {
        name: archiveName,
        size_bytes: statSync(archivePath).size,
        sha256: sha256File(archivePath),
      },
    };
    writeFileSync(
      path.join(publishRoot, "suite-update-result.json"),
      `${JSON.stringify(result, null, 2)}\n`,
    );
    return result;
  });
}

export function buildSuiteUpdatePackage({
  lockFilename,
  compositionDirectory,
  assetsRoot,
  outputRoot,
  platform,
  selectedComponents,
  diagnosticReview = false,
  metadataRoot = "",
}) {
  if (platform !== supportedPlatform) {
    throw new Error(`package-only updates currently require ${supportedPlatform}`);
  }
  const source = sourceIdentity();
  const inputs = loadReleaseInputs(lockFilename, compositionDirectory);
  const validation = validateReleaseLock(inputs.lock, inputs.composition, {
    requireReady: !diagnosticReview,
    requireNotReady: diagnosticReview,
    provenanceMode: diagnosticReview ? "diagnostic_review" : "official",
    requiredTargets: [platform],
    compositionFileName: inputs.compositionFileName,
    compositionSha256: inputs.compositionSha256,
  });
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  const transactionRoot = `${path.resolve(outputRoot)}.candidate-inputs-${process.pid}`;
  mkdirSync(transactionRoot, { recursive: true });
  try {
    const candidates = verifyAndExtractCandidates({
      lock: inputs.lock,
      composition: inputs.composition,
      target: platform,
      assetsRoot,
      workRoot: transactionRoot,
      cosignExecutable: resolveCosignExecutable({ repositoryRoot }),
    });
    return assembleSuiteUpdatePackage({
      composition: inputs.composition,
      candidates,
      selectedComponents,
      outputRoot,
      sourceCommit: source.commit,
      sourceEpoch: source.epoch,
      compositionSha256: inputs.compositionSha256,
      releaseLockSha256: sha256File(path.resolve(lockFilename)),
      inputAuthority: diagnosticReview ? "diagnostic_review_lock" : "release_ready_lock",
      metadataRoot,
    });
  } finally {
    // Candidate extraction is temporary and never release evidence.
    rmSync(transactionRoot, { recursive: true, force: true });
  }
}

function parse(argv) {
  const values = { component: [] };
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === "--diagnostic-review") {
      values.diagnosticReview = true;
    } else if (flag === "--component") {
      values.component.push(argv[++index]);
    } else if (flag.startsWith("--")) {
      values[flag.slice(2)] = argv[++index];
    } else {
      throw new Error(`unexpected argument: ${flag}`);
    }
  }
  if (values.component.length < 1) throw new Error("at least one --component is required");
  return {
    lockFilename: values.lock ?? path.join(repositoryRoot, "release", "catalog.lock.json"),
    compositionDirectory: values["composition-directory"]
      ?? path.join(repositoryRoot, "compositions"),
    assetsRoot: values["asset-root"]
      ?? path.join(repositoryRoot, "build", "release-inputs"),
    outputRoot: values["output-root"]
      ?? path.join(repositoryRoot, "dist", "suite-update"),
    platform: values.platform,
    selectedComponents: values.component,
    diagnosticReview: Boolean(values.diagnosticReview),
    metadataRoot: values["metadata-root"] ?? "",
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    const result = buildSuiteUpdatePackage(parse(process.argv.slice(2)));
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  } catch (error) {
    process.stderr.write(`build-suite-update-package: ${error.message}\n`);
    process.exitCode = 1;
  }
}
