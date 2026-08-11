#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  existsSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

import { validateProductComposition } from "./product-composition.mjs";
import { validateReleaseLock } from "./release-lock.mjs";

const COMMIT = /^[0-9a-f]{40}$/u;
const SAFE_ASSET = /^[A-Za-z0-9._-]+$/u;
const HEAD_REF = /^refs\/heads\/(?!.*\.\.)(?!.*\/\/)(?!.*[\/.]$)[A-Za-z0-9._\/-]+$/u;
const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function exactRegularFile(directory, name, label) {
  if (!SAFE_ASSET.test(name)) throw new Error(`${label} has an unsafe asset name`);
  const filename = path.join(path.resolve(directory), name);
  const status = lstatSync(filename);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`${label} must be a nonempty regular file`);
  }
  return { filename, size: status.size };
}

function workflowRef(result, repository, workflowPath) {
  const prefix = `${repository}/${workflowPath}@`;
  if (typeof result.workflow_identity !== "string"
      || !result.workflow_identity.startsWith(prefix)
      || !/#\d+\.\d+$/u.test(result.workflow_identity)) {
    throw new Error(`${result.component_id} workflow identity is invalid`);
  }
  const ref = result.workflow_identity.slice(prefix.length).replace(/#\d+\.\d+$/u, "");
  if (!HEAD_REF.test(ref)) {
    throw new Error(`${result.component_id} workflow identity is not an exact branch ref`);
  }
  return ref;
}

function sourceByComponent(sourceSet, composition) {
  if (sourceSet?.schema !== "arena_diagnostic_review_sources_v1"
      || sourceSet.suite_version !== composition.product_release
      || !["windows-x86_64", "linux-x86_64"].includes(sourceSet.target)
      || !Array.isArray(sourceSet.releases)) {
    throw new Error("diagnostic review source set identity is invalid");
  }
  const compositionById = new Map(
    composition.components.map((component) => [component.component_id, component]),
  );
  const byId = new Map();
  for (const row of sourceSet.releases) {
    const component = compositionById.get(row?.component_id);
    if (!component || byId.has(row.component_id)
        || row.repository !== component.repository
        || typeof row.release_tag !== "string" || row.release_tag.length === 0
        || !COMMIT.test(row.source_commit ?? "")) {
      throw new Error(`invalid diagnostic source for ${row?.component_id ?? "<unknown>"}`);
    }
    byId.set(row.component_id, row);
  }
  for (const component of composition.components) {
    if (!byId.has(component.component_id)) {
      throw new Error(`diagnostic source is missing ${component.component_id}`);
    }
  }
  if (byId.size !== composition.components.length) {
    throw new Error("diagnostic source set contains an undeclared component");
  }
  return byId;
}

function providerFor(baseLock, repository) {
  const entry = Object.entries(baseLock.providers ?? {}).find(
    ([, provider]) => provider.repository === repository,
  );
  if (!entry) throw new Error(`no trusted provider contract exists for ${repository}`);
  return { provider_id: entry[0], ...entry[1] };
}

function candidateAssets(directory, resultSuffix) {
  const names = readdirSync(directory).sort();
  if (names.length !== 3 || names.some((name) => !SAFE_ASSET.test(name))) {
    throw new Error("candidate release must contain exactly three safe assets");
  }
  const resultNames = names.filter((name) => name.endsWith(resultSuffix));
  if (resultNames.length !== 1) throw new Error(`candidate has no unique ${resultSuffix} asset`);
  const resultFile = exactRegularFile(directory, resultNames[0], "candidate result");
  const result = readJson(resultFile.filename, "candidate result");
  const artifactFile = exactRegularFile(directory, result.artifact_name, "candidate artifact");
  const bundleName = `${result.artifact_name}.sigstore.json`;
  const bundleFile = exactRegularFile(directory, bundleName, "candidate Sigstore bundle");
  return { result, resultFile, artifactFile, bundleName, bundleFile };
}

function commonRow({ component, source, assets, workflowName, workflowPath }) {
  const result = assets.result;
  if (result.component_id !== component.component_id
      || result.component_version !== component.version
      || result.target_triple !== source.target
      || result.source_repository !== component.repository
      || result.source_commit !== source.source_commit
      || result.artifact_sha256 !== sha256File(assets.artifactFile.filename)
      || result.artifact_size_bytes !== assets.artifactFile.size) {
    throw new Error(`${component.component_id} release result differs from its declared source`);
  }
  const ref = workflowRef(result, component.repository, workflowPath);
  return {
    component_id: component.component_id,
    component_version: component.version,
    target_triple: source.target,
    source_commit: source.source_commit,
    artifact_name: result.artifact_name,
    artifact_sha256: result.artifact_sha256,
    artifact_type: result.artifact_type
      ?? (result.artifact_name.endsWith(".file-set.json") ? "file_set" : "archive"),
    dependencies: [...component.requires],
    provenance_bundle_name: assets.bundleName,
    provenance_bundle_sha256: sha256File(assets.bundleFile.filename),
    provenance_certificate_identity:
      `https://github.com/${component.repository}/${workflowPath}@${ref}`,
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: workflowName,
    provenance_workflow_ref: ref,
    provenance_workflow_trigger: "workflow_dispatch",
  };
}

export function buildDiagnosticReviewLock({
  baseLock,
  composition: compositionCandidate,
  compositionFileName,
  compositionSha256,
  sourceSet,
  candidateDirectories,
}) {
  const composition = validateProductComposition(compositionCandidate);
  const sources = sourceByComponent(sourceSet, composition);
  const components = [];
  const managedComponents = [];
  for (const component of composition.components) {
    const source = { ...sources.get(component.component_id), target: sourceSet.target };
    const directory = candidateDirectories.get(component.component_id);
    if (!directory) throw new Error(`candidate directory is missing ${component.component_id}`);
    if (component.repository === INSTALLER_REPOSITORY) {
      const assets = candidateAssets(directory, ".managed-result.json");
      const row = commonRow({
        component, source, assets,
        workflowName: "Managed component candidate",
        workflowPath: ".github/workflows/managed-component-release.yml",
      });
      managedComponents.push({
        ...row,
        managed_release_tag: source.release_tag,
        managed_result_name: path.basename(assets.resultFile.filename),
        managed_result_sha256: sha256File(assets.resultFile.filename),
        artifact_size_bytes: assets.artifactFile.size,
        installed_bytes: assets.result.installed_bytes,
        component_manifest_sha256: assets.result.component_manifest_sha256,
        licenses_sha256: assets.result.licenses_sha256,
      });
    } else {
      const provider = providerFor(baseLock, component.repository);
      const assets = candidateAssets(directory, ".provider-result.json");
      const row = commonRow({
        component, source, assets,
        workflowName: provider.workflow_name,
        workflowPath: provider.workflow_path,
      });
      components.push({
        provider_id: provider.provider_id,
        ...row,
        provider_release_tag: source.release_tag,
        provider_result_name: path.basename(assets.resultFile.filename),
        provider_result_sha256: sha256File(assets.resultFile.filename),
        file_count: assets.result.file_set_file_count,
        installed_bytes: assets.result.file_set_total_size_bytes,
      });
    }
  }
  const lock = {
    schema: "arena_suite_release_lock_v2",
    suite_version: composition.product_release,
    release_ready: false,
    composition: { manifest_name: compositionFileName, sha256: compositionSha256 },
    providers: structuredClone(baseLock.providers),
    provider_asset_mirror: {
      repository: INSTALLER_REPOSITORY,
      release_tag: `provider-mirror/v${composition.product_release}`,
    },
    managed_repository: INSTALLER_REPOSITORY,
    components,
    managed_components: managedComponents,
  };
  const validation = validateReleaseLock(lock, composition, {
    requireReady: false,
    requireNotReady: true,
    provenanceMode: "diagnostic_review",
    requiredTargets: [sourceSet.target],
    compositionFileName,
    compositionSha256,
  });
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  return lock;
}

function executeGh(args, { capture = false } = {}) {
  const result = spawnSync("gh", args, {
    encoding: "utf8",
    stdio: capture ? "pipe" : "inherit",
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    const diagnostic = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim().length > 0)
      .join("\n");
    throw new Error(`gh ${args.slice(0, 3).join(" ")} failed: ${diagnostic}`);
  }
  return result.stdout ?? "";
}

function acquireRelease(source, destination, visibilityCache) {
  let visibility = visibilityCache.get(source.repository);
  if (!visibility) {
    visibility = JSON.parse(executeGh([
      "repo", "view", source.repository, "--json", "visibility",
    ], { capture: true })).visibility;
    visibilityCache.set(source.repository, visibility);
  }
  if (visibility !== "PRIVATE") {
    throw new Error(`diagnostic provider repository must remain private: ${source.repository}`);
  }
  const release = JSON.parse(executeGh([
    "release", "view", source.release_tag, "--repo", source.repository,
    "--json", "isDraft,isPrerelease,targetCommitish,assets",
  ], { capture: true }));
  if (release.isDraft || !release.isPrerelease
      || release.targetCommitish !== source.source_commit
      || !Array.isArray(release.assets) || release.assets.length !== 3
      || release.assets.some((asset) => !SAFE_ASSET.test(asset.name))) {
    throw new Error(`${source.component_id} release is not an exact immutable private candidate`);
  }
  mkdirSync(destination, { recursive: false });
  executeGh([
    "release", "download", source.release_tag,
    "--repo", source.repository,
    "--dir", destination,
  ]);
}

export function prepareDiagnosticReviewStaging(stagingRoot) {
  rmSync(stagingRoot, { recursive: true, force: true });
  mkdirSync(stagingRoot, { recursive: false });
  mkdirSync(path.join(stagingRoot, "assets"), { recursive: false });
}

export function acquireDiagnosticReviewInputs({
  baseLock,
  composition,
  compositionFileName,
  compositionSha256,
  sourceSet,
  outputRoot,
}) {
  const output = path.resolve(outputRoot);
  if (output === path.parse(output).root) throw new Error("review output cannot be a filesystem root");
  const sources = sourceByComponent(sourceSet, validateProductComposition(composition));
  const staging = `${output}.staging-${process.pid}`;
  const previous = `${output}.previous-${process.pid}`;
  prepareDiagnosticReviewStaging(staging);
  rmSync(previous, { recursive: true, force: true });
  try {
    const candidateDirectories = new Map();
    const visibilityCache = new Map();
    for (const component of composition.components) {
      const destination = path.join(staging, "assets", component.component_id);
      acquireRelease(sources.get(component.component_id), destination, visibilityCache);
      candidateDirectories.set(component.component_id, destination);
    }
    const lock = buildDiagnosticReviewLock({
      baseLock, composition, compositionFileName, compositionSha256,
      sourceSet, candidateDirectories,
    });
    writeFileSync(path.join(staging, "review-lock.json"), `${JSON.stringify(lock, null, 2)}\n`);
    writeFileSync(path.join(staging, "review-sources.json"), `${JSON.stringify(sourceSet, null, 2)}\n`);
    const hadPrevious = existsSync(output);
    if (hadPrevious) renameSync(output, previous);
    try {
      renameSync(staging, output);
      if (hadPrevious) rmSync(previous, { recursive: true, force: true });
    } catch (error) {
      if (existsSync(output)) rmSync(output, { recursive: true, force: true });
      if (hadPrevious && existsSync(previous)) renameSync(previous, output);
      throw error;
    }
    return {
      schema: "arena_diagnostic_review_inputs_v1",
      suite_version: composition.product_release,
      target: sourceSet.target,
      components: composition.components.length,
      root: output,
      lock: path.join(output, "review-lock.json"),
    };
  } finally {
    rmSync(staging, { recursive: true, force: true });
  }
}

function parse(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    if (!argv[index]?.startsWith("--") || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${argv[index] ?? "<end>"}`);
    }
    values[argv[index].slice(2)] = argv[index + 1];
  }
  for (const required of ["sources", "composition", "base-lock", "output-root"]) {
    if (!values[required]) throw new Error(`missing --${required}`);
  }
  return values;
}

function main(argv) {
  const args = parse(argv);
  const compositionPath = path.resolve(args.composition);
  const result = acquireDiagnosticReviewInputs({
    baseLock: readJson(path.resolve(args["base-lock"]), "base release lock"),
    composition: readJson(compositionPath, "product composition"),
    compositionFileName: path.basename(compositionPath),
    compositionSha256: sha256File(compositionPath),
    sourceSet: readJson(path.resolve(args.sources), "diagnostic source set"),
    outputRoot: args["output-root"],
  });
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`diagnostic-review-inputs: ${error.message}\n`);
    process.exitCode = 1;
  }
}
