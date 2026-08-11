#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";

import { validateProductComposition } from "./product-composition.mjs";

const DIGEST = /^[0-9a-f]{64}$/u;
const SOURCE_COMMIT = /^[0-9a-f]{40}$/u;
const SAFE_NAME = /^[A-Za-z0-9._-]+$/u;
const TARGETS = new Set(["windows-x86_64", "linux-x86_64", "any", "any-any"]);
const PLATFORM_TARGETS = ["windows-x86_64", "linux-x86_64"];
const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";
const HEAD_REF = /^refs\/heads\/(?!.*\.\.)(?!.*\/\/)(?!.*[\/.]$)[A-Za-z0-9._\/-]+$/u;

export function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function isObject(value) {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}

function sameStrings(left, right) {
  return Array.isArray(left)
    && Array.isArray(right)
    && left.length === right.length
    && [...left].sort().every((value, index) => value === [...right].sort()[index]);
}

function requiredText(row, field, context, errors, pattern = undefined) {
  const value = row?.[field];
  if (typeof value !== "string" || value.length === 0 || (pattern && !pattern.test(value))) {
    errors.push(`${context}.${field} is invalid`);
    return "";
  }
  return value;
}

function validateArtifactIdentity(row, context, errors) {
  const artifactName = requiredText(row, "artifact_name", context, errors);
  if (artifactName && (!SAFE_NAME.test(artifactName.replace(/\.tar\.gz$/u, ""))
      || !(artifactName.endsWith(".tar.gz") || artifactName.endsWith(".file-set.json")))) {
    errors.push(`${context}.artifact_name is not a supported safe asset name`);
  }
  requiredText(row, "artifact_sha256", context, errors, DIGEST);
  if (!Array.isArray(row.dependencies)
      || new Set(row.dependencies).size !== row.dependencies.length
      || row.dependencies.some((dependency) => typeof dependency !== "string" || !SAFE_NAME.test(dependency))) {
    errors.push(`${context}.dependencies must contain unique component ids`);
  }
}

function validateProvenance(row, provider, context, errors, provenanceMode) {
  const workflowRef = requiredText(row, "provenance_workflow_ref", context, errors);
  if (provenanceMode === "official") {
    if (workflowRef !== "refs/heads/main") {
      errors.push(`${context} is not a main-branch candidate`);
    }
  } else if (provenanceMode === "diagnostic_review") {
    if (!HEAD_REF.test(workflowRef)) {
      errors.push(`${context}.provenance_workflow_ref is not an exact branch ref`);
    }
  } else {
    errors.push(`unsupported provenance validation mode: ${provenanceMode}`);
  }
  const expectedIdentity =
    `https://github.com/${provider.repository}/${provider.workflow_path}@${workflowRef}`;
  if (row.provenance_certificate_identity !== expectedIdentity) {
    errors.push(`${context}.provenance_certificate_identity does not match its provider workflow`);
  }
  if (row.provenance_oidc_issuer !== "https://token.actions.githubusercontent.com") {
    errors.push(`${context}.provenance_oidc_issuer is invalid`);
  }
  if (row.provenance_workflow_name !== provider.workflow_name) {
    errors.push(`${context}.provenance_workflow_name does not match its provider`);
  }
  if (row.provenance_workflow_trigger !== "workflow_dispatch") {
    errors.push(`${context} is not an explicitly dispatched candidate`);
  }
  requiredText(row, "provenance_bundle_name", context, errors);
  requiredText(row, "provenance_bundle_sha256", context, errors, DIGEST);
}

function providerByRepository(lock, errors) {
  if (!isObject(lock.providers)) {
    errors.push("providers must be an object");
    return new Map();
  }
  const byRepository = new Map();
  for (const [providerId, provider] of Object.entries(lock.providers)) {
    const context = `providers.${providerId}`;
    if (!/^[a-z][a-z0-9_]*$/u.test(providerId)) {
      errors.push(`${context} id is invalid`);
    }
    if (!isObject(provider)) {
      errors.push(`${context} must be an object`);
      continue;
    }
    const repository = requiredText(provider, "repository", context, errors);
    if (provider.workflow_path !== ".github/workflows/component-release.yml") {
      errors.push(`${context}.workflow_path is not the component-release contract`);
    }
    requiredText(provider, "workflow_name", context, errors);
    if (repository && byRepository.has(repository)) {
      errors.push(`provider repository is duplicated: ${repository}`);
    } else if (repository) {
      byRepository.set(repository, { id: providerId, ...provider });
    }
  }
  return byRepository;
}

function validateProviderRow(
  row, index, compositionById, providers, errors, provenanceMode,
) {
  const context = `components[${index}]`;
  if (!isObject(row)) {
    errors.push(`${context} must be an object`);
    return null;
  }
  const id = requiredText(row, "component_id", context, errors, SAFE_NAME);
  const component = compositionById.get(id);
  if (!component) {
    errors.push(`${context} is not declared by the product composition`);
    return null;
  }
  if (component.repository === INSTALLER_REPOSITORY) {
    errors.push(`${context} is Installer-managed and belongs in managed_components`);
  }
  const provider = providers.get(component.repository);
  if (!provider || row.provider_id !== provider.id) {
    errors.push(`${context}.provider_id does not resolve to ${component.repository}`);
  }
  if (row.component_version !== component.version) {
    errors.push(`${context}.component_version differs from the product composition`);
  }
  if (!TARGETS.has(row.target_triple)) errors.push(`${context}.target_triple is invalid`);
  if (!sameStrings(row.dependencies, component.requires)) {
    errors.push(`${context}.dependencies differ from the product composition`);
  }
  requiredText(row, "source_commit", context, errors, SOURCE_COMMIT);
  requiredText(row, "provider_release_tag", context, errors);
  requiredText(row, "provider_result_name", context, errors);
  requiredText(row, "provider_result_sha256", context, errors, DIGEST);
  validateArtifactIdentity(row, context, errors);
  if (provider) validateProvenance(row, provider, context, errors, provenanceMode);
  return { id, target: row.target_triple };
}

function validateManagedRow(row, index, compositionById, errors, provenanceMode) {
  const context = `managed_components[${index}]`;
  if (!isObject(row)) {
    errors.push(`${context} must be an object`);
    return null;
  }
  const id = requiredText(row, "component_id", context, errors, SAFE_NAME);
  const component = compositionById.get(id);
  if (!component) {
    errors.push(`${context} is not declared by the product composition`);
    return null;
  }
  if (component.repository !== INSTALLER_REPOSITORY) {
    errors.push(`${context} is provider-owned and belongs in components`);
  }
  if (row.component_version !== component.version) {
    errors.push(`${context}.component_version differs from the product composition`);
  }
  if (!TARGETS.has(row.target_triple)) errors.push(`${context}.target_triple is invalid`);
  if (!sameStrings(row.dependencies, component.requires)) {
    errors.push(`${context}.dependencies differ from the product composition`);
  }
  requiredText(row, "source_commit", context, errors, SOURCE_COMMIT);
  requiredText(row, "managed_release_tag", context, errors);
  requiredText(row, "managed_result_name", context, errors);
  requiredText(row, "managed_result_sha256", context, errors, DIGEST);
  for (const field of ["artifact_size_bytes", "installed_bytes"]) {
    if (!Number.isSafeInteger(row[field]) || row[field] < 1) {
      errors.push(`${context}.${field} must be a positive safe integer`);
    }
  }
  requiredText(row, "component_manifest_sha256", context, errors, DIGEST);
  requiredText(row, "licenses_sha256", context, errors, DIGEST);
  validateArtifactIdentity(row, context, errors);
  validateProvenance(row, {
    repository: INSTALLER_REPOSITORY,
    workflow_path: ".github/workflows/managed-component-release.yml",
    workflow_name: "Managed component candidate",
  }, context, errors, provenanceMode);
  return { id, target: row.target_triple };
}

function validateCoverage(composition, identities, errors, requiredTargets) {
  const targetsById = new Map();
  for (const identity of identities.filter(Boolean)) {
    const targets = targetsById.get(identity.id) ?? new Set();
    if (targets.has(identity.target)) {
      errors.push(`candidate identity is duplicated: ${identity.id}:${identity.target}`);
    }
    targets.add(identity.target);
    targetsById.set(identity.id, targets);
  }
  for (const component of composition.components) {
    const targets = targetsById.get(component.component_id) ?? new Set();
    const portable = targets.has("any") || targets.has("any-any");
    if (portable && targets.size !== 1) {
      errors.push(`${component.component_id} mixes portable and platform candidates`);
      continue;
    }
    if (!portable) {
      for (const target of requiredTargets) {
        if (!targets.has(target)) errors.push(`${component.component_id} is missing ${target}`);
      }
      for (const target of targets) {
        if (!PLATFORM_TARGETS.includes(target)) {
          errors.push(`${component.component_id} has unsupported coverage ${target}`);
        }
      }
    }
  }
}

export function validateReleaseLock(lock, compositionCandidate, {
  requireReady = true,
  requireNotReady = false,
  provenanceMode = "official",
  requiredTargets = undefined,
  compositionFileName = undefined,
  compositionSha256 = undefined,
} = {}) {
  const errors = [];
  let composition;
  try {
    composition = validateProductComposition(compositionCandidate);
  } catch (error) {
    return { ok: false, errors: [`invalid product composition: ${error.message}`] };
  }
  if (!isObject(lock) || lock.schema !== "arena_suite_release_lock_v2") {
    return { ok: false, errors: ["release lock schema is unsupported"] };
  }
  if (lock.suite_version !== composition.product_release) {
    errors.push("suite_version differs from product composition");
  }
  if (typeof lock.release_ready !== "boolean") errors.push("release_ready must be boolean");
  if (requireReady && lock.release_ready !== true) errors.push("release lock is not release-ready");
  if (requireNotReady && lock.release_ready !== false) {
    errors.push("diagnostic review lock must not be release-ready");
  }
  if (requiredTargets !== undefined
      && (!Array.isArray(requiredTargets)
        || requiredTargets.length === 0
        || requiredTargets.some((target) => !PLATFORM_TARGETS.includes(target)))) {
    errors.push("requiredTargets must contain supported platform targets");
  }
  if (!isObject(lock.composition)) {
    errors.push("composition identity is required");
  } else {
    if (compositionFileName && lock.composition.manifest_name !== compositionFileName) {
      errors.push("composition manifest_name differs from the loaded file");
    }
    if (compositionSha256 && lock.composition.sha256 !== compositionSha256) {
      errors.push("composition sha256 differs from the loaded file");
    }
    requiredText(lock.composition, "manifest_name", "composition", errors);
    requiredText(lock.composition, "sha256", "composition", errors, DIGEST);
  }
  if (lock.managed_repository !== INSTALLER_REPOSITORY) {
    errors.push(`managed_repository must be ${INSTALLER_REPOSITORY}`);
  }
  if (!isObject(lock.provider_asset_mirror)
      || lock.provider_asset_mirror.repository !== INSTALLER_REPOSITORY
      || lock.provider_asset_mirror.release_tag !== `provider-mirror/v${lock.suite_version}`) {
    errors.push("provider_asset_mirror does not match Installer ownership and suite version");
  }

  const providers = providerByRepository(lock, errors);
  const compositionProviderRepositories = new Set(
    composition.components
      .map((component) => component.repository)
      .filter((repository) => repository !== INSTALLER_REPOSITORY),
  );
  for (const repository of providers.keys()) {
    if (!compositionProviderRepositories.has(repository)) {
      errors.push(`provider repository is not used by the product composition: ${repository}`);
    }
  }
  for (const component of composition.components) {
    if (component.repository !== INSTALLER_REPOSITORY && !providers.has(component.repository)) {
      errors.push(`no provider contract exists for ${component.repository}`);
    }
  }
  const compositionById = new Map(
    composition.components.map((component) => [component.component_id, component]),
  );
  if (!Array.isArray(lock.components)) errors.push("components must be an array");
  if (!Array.isArray(lock.managed_components)) errors.push("managed_components must be an array");
  const identities = [
    ...(Array.isArray(lock.components) ? lock.components.map((row, index) =>
      validateProviderRow(
        row, index, compositionById, providers, errors, provenanceMode,
      )) : []),
    ...(Array.isArray(lock.managed_components) ? lock.managed_components.map((row, index) =>
      validateManagedRow(row, index, compositionById, errors, provenanceMode)) : []),
  ];
  const coverage = requiredTargets ?? (lock.release_ready === true ? PLATFORM_TARGETS : []);
  if (coverage.length > 0) validateCoverage(composition, identities, errors, coverage);
  return { ok: errors.length === 0, errors };
}

export function loadReleaseInputs(lockFilename, compositionDirectory) {
  const lockPath = path.resolve(lockFilename);
  const lock = JSON.parse(readFileSync(lockPath, "utf8"));
  const manifestName = lock?.composition?.manifest_name;
  if (typeof manifestName !== "string" || path.basename(manifestName) !== manifestName) {
    throw new Error("release lock composition manifest_name must be a filename");
  }
  const root = path.resolve(compositionDirectory);
  const compositionPath = path.resolve(root, manifestName);
  if (path.dirname(compositionPath) !== root) {
    throw new Error("release lock composition escaped the composition directory");
  }
  return {
    lock,
    composition: JSON.parse(readFileSync(compositionPath, "utf8")),
    compositionFileName: manifestName,
    compositionSha256: sha256File(compositionPath),
  };
}

function main(argv) {
  const allowDraft = argv.includes("--allow-draft");
  const positional = argv.filter((argument) => argument !== "--allow-draft");
  if (positional.length > 2) {
    throw new Error("usage: release-lock.mjs [--allow-draft] [lock.json] [composition-directory]");
  }
  const lockPath = path.resolve(positional[0] ?? "release/catalog.lock.json");
  const compositionDirectory = path.resolve(positional[1] ?? "compositions");
  const inputs = loadReleaseInputs(lockPath, compositionDirectory);
  const result = validateReleaseLock(inputs.lock, inputs.composition, {
    requireReady: !allowDraft,
    compositionFileName: inputs.compositionFileName,
    compositionSha256: inputs.compositionSha256,
  });
  if (!result.ok) throw new Error(result.errors.join("\n"));
  process.stdout.write(
    `${inputs.lock.suite_version}: ${inputs.lock.release_ready ? "release-ready" : "draft"} lock valid\n`,
  );
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`release lock: ${error.message}\n`);
    process.exitCode = 1;
  }
}
