#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";

import { validateProductComposition } from "./product-composition.mjs";
import { validateProvenanceTrustPolicy } from "./provenance-trust-policy.mjs";

const DIGEST = /^[0-9a-f]{64}$/u;
const SOURCE_COMMIT = /^[0-9a-f]{40}$/u;
const SAFE_NAME = /^[A-Za-z0-9._-]+$/u;
const TARGETS = new Set(["windows-x86_64", "linux-x86_64", "any", "any-any"]);
const PLATFORM_TARGETS = ["windows-x86_64", "linux-x86_64"];
const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";
const HEAD_REF = /^refs\/heads\/(?!.*\.\.)(?!.*\/\/)(?!.*[\/.]$)[A-Za-z0-9._\/-]+$/u;
const SEMVER = /^[0-9]+\.[0-9]+\.[0-9]+$/u;
const IDENTIFIER = /^[A-Za-z0-9._-]{1,128}$/u;
const COMPONENT_VERSION = /^[A-Za-z0-9._-]{1,64}$/u;
const REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const V3_SOURCE_COMMIT = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/u;
const V3_TARGETS = new Set(["windows-x86_64", "linux-x86_64", "any"]);
const OWNERSHIP = new Set(["provider", "installer_managed"]);
const EVIDENCE_FORMATS = new Set([
  "provider_result_v1", "managed_result_v1", "component_evidence_v1",
]);
const V3_ROOT_FIELDS = [
  "schema", "suite_version", "release_ready", "validation_mode",
  "composition", "trust_policy", "components",
];
const V3_ROW_FIELDS = [
  "component_id", "component_version", "target_triple", "ownership",
  "source_repository", "source_commit", "artifact", "component_manifest_sha256",
  "dependencies", "evidence", "provenance",
];
const ARTIFACT_FIELDS = [
  "name", "sha256", "size_bytes", "type", "file_count", "installed_bytes",
];
const ARTIFACT_REQUIRED_FIELDS = ["name", "sha256", "size_bytes", "type"];
const ARTIFACT_BYTES_MAX = 1_099_511_627_776;
const V3_ROW_LIMIT = 512;
const VALIDATION_ERROR_LIMIT = 64;

class BoundedErrors {
  #errors = [];

  #omitted = false;

  push(...messages) {
    for (const message of messages) {
      if (this.#errors.length < VALIDATION_ERROR_LIMIT - 1) {
        this.#errors.push(String(message).slice(0, 2048));
      } else {
        this.#omitted = true;
      }
    }
  }

  get length() {
    return this.#errors.length + (this.#omitted ? 1 : 0);
  }

  values() {
    return this.#omitted
      ? [...this.#errors, "additional validation errors omitted"]
      : [...this.#errors];
  }
}

function validationResult(errors, compatibility = undefined) {
  const values = errors instanceof BoundedErrors ? errors.values() : errors;
  const result = { ok: values.length === 0, errors: values };
  if (compatibility !== undefined) {
    Object.defineProperty(result, "compatibility", {
      value: compatibility,
      enumerable: false,
    });
  }
  return result;
}

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

function exactFields(value, allowed, required, context, errors) {
  if (!isObject(value)) {
    errors.push(`${context} must be an object`);
    return false;
  }
  const allowedSet = new Set(allowed);
  const actual = Object.keys(value);
  if (actual.some((field) => !allowedSet.has(field))
      || required.some((field) => !Object.hasOwn(value, field))) {
    errors.push(`${context} fields are invalid`);
    return false;
  }
  return true;
}

function validText(value, pattern, maximum = Infinity) {
  return typeof value === "string" && value.length <= maximum && pattern.test(value);
}

function safeAssetName(value) {
  return validText(value, SAFE_NAME, 255);
}

function validateV3Root(lock, errors) {
  exactFields(lock, V3_ROOT_FIELDS, V3_ROOT_FIELDS, "root", errors);
  if (lock.schema !== "arena_suite_release_lock_v3") {
    errors.push("release lock schema is unsupported");
  }
  if (!validText(lock.suite_version, SEMVER)) {
    errors.push("suite_version must be semantic version text");
  }
  if (typeof lock.release_ready !== "boolean") {
    errors.push("release_ready must be boolean");
  }
  if (!new Set(["official", "diagnostic_review"]).has(lock.validation_mode)) {
    errors.push("validation_mode is invalid");
  }
  if (!Array.isArray(lock.components)
      || lock.components.length < 1
      || lock.components.length > V3_ROW_LIMIT) {
    errors.push("components must contain 1 to 512 rows");
  }
}

function validateRequiredTargets(requiredTargets, releaseReady, validationMode, errors) {
  if (requiredTargets === undefined) {
    if (releaseReady === true && validationMode === "official") {
      errors.push("requiredTargets must be nonempty for an official release-ready lock");
    }
    return [];
  }
  if (!Array.isArray(requiredTargets)
      || requiredTargets.length > PLATFORM_TARGETS.length
      || new Set(requiredTargets).size !== requiredTargets.length
      || requiredTargets.some((target) => !PLATFORM_TARGETS.includes(target))) {
    errors.push("requiredTargets must contain unique supported platform targets");
    return [];
  }
  if (requiredTargets.length === 0
      && releaseReady === true
      && validationMode === "official") {
    errors.push("requiredTargets must be nonempty for an official release-ready lock");
  }
  return [...requiredTargets];
}

function validateV3Bindings(lock, composition, options, errors) {
  if (lock.suite_version !== composition.product_release) {
    errors.push("suite_version differs from product composition");
  }

  const compositionFields = ["manifest_name", "sha256"];
  if (exactFields(
    lock.composition, compositionFields, compositionFields, "composition", errors,
  )) {
    if (!validText(
      lock.composition.manifest_name,
      /^complete-arena-[0-9]+\.[0-9]+\.[0-9]+\.json$/u,
      255,
    )) {
      errors.push("composition.manifest_name is invalid");
    }
    if (!validText(lock.composition.sha256, DIGEST)) {
      errors.push("composition.sha256 is invalid");
    }
    if (validText(lock.suite_version, SEMVER)
        && lock.composition.manifest_name !== `complete-arena-${lock.suite_version}.json`) {
      errors.push("composition manifest_name differs from suite_version");
    }
  }
  if (!safeAssetName(options.compositionFileName)
      || !/^complete-arena-[0-9]+\.[0-9]+\.[0-9]+\.json$/u.test(options.compositionFileName)) {
    errors.push("compositionFileName is required and invalid");
  } else if (lock.composition?.manifest_name !== options.compositionFileName) {
    errors.push("composition manifest_name differs from the loaded file");
  }
  if (!validText(options.compositionSha256, DIGEST)) {
    errors.push("compositionSha256 is required and invalid");
  } else if (lock.composition?.sha256 !== options.compositionSha256) {
    errors.push("composition sha256 differs from the loaded file");
  }

  const policyFields = ["name", "sha256"];
  if (exactFields(lock.trust_policy, policyFields, policyFields, "trust_policy", errors)) {
    if (!safeAssetName(lock.trust_policy.name)) errors.push("trust_policy.name is invalid");
    if (!validText(lock.trust_policy.sha256, DIGEST)) {
      errors.push("trust_policy.sha256 is invalid");
    }
  }
  if (!safeAssetName(options.trustPolicyFileName)) {
    errors.push("trustPolicyFileName is required and invalid");
  } else if (lock.trust_policy?.name !== options.trustPolicyFileName) {
    errors.push("trust_policy name differs from the loaded file");
  }
  if (!validText(options.trustPolicySha256, DIGEST)) {
    errors.push("trustPolicySha256 is required and invalid");
  } else if (lock.trust_policy?.sha256 !== options.trustPolicySha256) {
    errors.push("trust_policy sha256 differs from the loaded file");
  }

  try {
    return validateProvenanceTrustPolicy(options.trustPolicy);
  } catch (error) {
    errors.push(error instanceof Error ? error.message : "invalid provenance trust policy");
    return null;
  }
}

function validateV3Artifact(artifact, context, errors, assetNames) {
  if (!exactFields(
    artifact, ARTIFACT_FIELDS, ARTIFACT_REQUIRED_FIELDS, context, errors,
  )) return;
  if (!safeAssetName(artifact.name)) {
    errors.push(`${context}.name is invalid`);
  } else {
    assetNames.push({ name: artifact.name, context: `${context}.name` });
  }
  if (!validText(artifact.sha256, DIGEST)) errors.push(`${context}.sha256 is invalid`);
  if (!Number.isSafeInteger(artifact.size_bytes)
      || artifact.size_bytes < 1
      || artifact.size_bytes > ARTIFACT_BYTES_MAX) {
    errors.push(`${context}.size_bytes is invalid`);
  }
  if (!new Set(["archive", "file_set"]).has(artifact.type)) {
    errors.push(`${context}.type is invalid`);
    return;
  }
  if (artifact.type === "file_set") {
    for (const field of ["file_count", "installed_bytes"]) {
      if (!Number.isSafeInteger(artifact[field])
          || artifact[field] < 1
          || (field === "installed_bytes" && artifact[field] > ARTIFACT_BYTES_MAX)) {
        errors.push(`${context}.${field} is required and invalid for file_set`);
      }
    }
  } else if (Object.hasOwn(artifact, "file_count")
      || Object.hasOwn(artifact, "installed_bytes")) {
    errors.push(`${context} archive must not contain file-set metrics`);
  }
}

function validateV3Evidence(evidence, context, errors, assetNames) {
  const fields = ["format", "name", "sha256"];
  if (!exactFields(evidence, fields, fields, context, errors)) return;
  if (!EVIDENCE_FORMATS.has(evidence.format)) errors.push(`${context}.format is invalid`);
  if (!safeAssetName(evidence.name)) {
    errors.push(`${context}.name is invalid`);
  } else {
    assetNames.push({ name: evidence.name, context: `${context}.name` });
  }
  if (!validText(evidence.sha256, DIGEST)) errors.push(`${context}.sha256 is invalid`);
}

function validateV3Provenance(provenance, context, errors, assetNames) {
  const fields = ["producer_id", "bundle_name", "bundle_sha256"];
  if (!exactFields(provenance, fields, fields, context, errors)) return;
  if (!validText(provenance.producer_id, IDENTIFIER)) {
    errors.push(`${context}.producer_id is invalid`);
  }
  if (!safeAssetName(provenance.bundle_name)) {
    errors.push(`${context}.bundle_name is invalid`);
  } else {
    assetNames.push({ name: provenance.bundle_name, context: `${context}.bundle_name` });
  }
  if (!validText(provenance.bundle_sha256, DIGEST)) {
    errors.push(`${context}.bundle_sha256 is invalid`);
  }
}

function validateV3ProducerBinding(row, producerById, context, errors) {
  const producer = producerById.get(row.provenance?.producer_id);
  if (!producer) {
    errors.push(`${context}.provenance.producer_id is unknown`);
    return null;
  }
  const authorized = producer.scopes.some((scope) =>
    scope.source_repository === row.source_repository
      && scope.ownership === row.ownership
      && scope.component_ids.includes(row.component_id));
  if (!authorized) errors.push(`${context} is outside the selected producer scope`);
  if (producer.evidence_format !== row.evidence?.format) {
    errors.push(`${context}.evidence.format differs from the selected producer`);
  }
  if (producer.kind === "sigstore_keyless_github"
      && !new Set(["provider_result_v1", "managed_result_v1"]).has(row.evidence?.format)) {
    errors.push(`${context} GitHub producer evidence format is invalid`);
  }
  if (producer.kind === "sigstore_keyed_local"
      && row.evidence?.format !== "component_evidence_v1") {
    errors.push(`${context} local producer evidence format is invalid`);
  }
  return producer;
}

function validateV3Row(row, index, compositionById, producerById, errors) {
  const context = `components[${index}]`;
  const assetNames = [];
  if (!exactFields(row, V3_ROW_FIELDS, V3_ROW_FIELDS, context, errors)) {
    return { identity: null, assetNames, producer: null };
  }
  if (!validText(row.component_id, IDENTIFIER)) {
    errors.push(`${context}.component_id is invalid`);
  }
  if (!validText(row.component_version, COMPONENT_VERSION)) {
    errors.push(`${context}.component_version is invalid`);
  }
  if (!V3_TARGETS.has(row.target_triple)) errors.push(`${context}.target_triple is invalid`);
  if (!OWNERSHIP.has(row.ownership)) errors.push(`${context}.ownership is invalid`);
  if (!validText(row.source_repository, REPOSITORY, 256)) {
    errors.push(`${context}.source_repository is invalid`);
  }
  if (!validText(row.source_commit, V3_SOURCE_COMMIT)) {
    errors.push(`${context}.source_commit is invalid`);
  }
  if (!validText(row.component_manifest_sha256, DIGEST)) {
    errors.push(`${context}.component_manifest_sha256 is invalid`);
  }
  const dependenciesValid = Array.isArray(row.dependencies)
    && row.dependencies.length <= 256
    && new Set(row.dependencies).size === row.dependencies.length
    && row.dependencies.every((dependency) => validText(dependency, IDENTIFIER));
  if (!dependenciesValid) errors.push(`${context}.dependencies are invalid`);

  const component = compositionById.get(row.component_id);
  if (!component) {
    errors.push(`${context} is not declared by the product composition`);
  } else {
    if (row.component_version !== component.version) {
      errors.push(`${context}.component_version differs from the product composition`);
    }
    if (row.source_repository !== component.repository) {
      errors.push(`${context}.source_repository differs from the product composition`);
    }
    const expectedOwnership = component.repository === INSTALLER_REPOSITORY
      ? "installer_managed"
      : "provider";
    if (row.ownership !== expectedOwnership) {
      errors.push(`${context}.ownership differs from the product composition`);
    }
    if (dependenciesValid && !sameStrings(row.dependencies, component.requires)) {
      errors.push(`${context}.dependencies differ from the product composition`);
    }
  }

  validateV3Artifact(row.artifact, `${context}.artifact`, errors, assetNames);
  validateV3Evidence(row.evidence, `${context}.evidence`, errors, assetNames);
  validateV3Provenance(row.provenance, `${context}.provenance`, errors, assetNames);
  const producer = validateV3ProducerBinding(row, producerById, context, errors);
  const identity = validText(row.component_id, IDENTIFIER) && V3_TARGETS.has(row.target_triple)
    ? { componentId: row.component_id, target: row.target_triple }
    : null;
  return { identity, assetNames, producer };
}

function validateV3UniquenessAndCoverage(
  composition, rowResults, requiredTargets, errors,
) {
  const pairs = new Set();
  const targetsByComponent = new Map();
  const names = new Map();
  for (const result of rowResults) {
    if (result.identity) {
      const pair = `${result.identity.componentId}\0${result.identity.target}`;
      if (pairs.has(pair)) {
        errors.push(
          `candidate identity is duplicated: ${result.identity.componentId}:${result.identity.target}`,
        );
      }
      pairs.add(pair);
      const targets = targetsByComponent.get(result.identity.componentId) ?? new Set();
      targets.add(result.identity.target);
      targetsByComponent.set(result.identity.componentId, targets);
    }
    for (const asset of result.assetNames) {
      if (names.has(asset.name)) {
        errors.push(`asset name is duplicated globally: ${asset.name}`);
      } else {
        names.set(asset.name, asset.context);
      }
    }
  }

  for (const component of composition.components) {
    const targets = targetsByComponent.get(component.component_id) ?? new Set();
    if (targets.size === 0) {
      errors.push(`${component.component_id} has no candidate row`);
      continue;
    }
    if (targets.has("any") && targets.size > 1) {
      errors.push(`${component.component_id} mixes portable and platform candidates`);
      continue;
    }
    if (!targets.has("any")) {
      for (const target of requiredTargets) {
        if (!targets.has(target)) errors.push(`${component.component_id} is missing ${target}`);
      }
    }
  }
}

function validateV3Mode(lock, options, selectedProducers, errors) {
  if (typeof options.requireReady !== "boolean") errors.push("requireReady must be boolean");
  if (typeof options.requireNotReady !== "boolean") {
    errors.push("requireNotReady must be boolean");
  }
  if (!new Set(["official", "diagnostic_review"]).has(options.validationMode)) {
    errors.push("validationMode option is required and invalid");
  } else if (lock.validation_mode !== options.validationMode) {
    errors.push("validationMode option differs from the release lock");
  }
  if (options.requireReady && lock.release_ready !== true) {
    errors.push("release lock is not release-ready");
  }
  if (options.requireNotReady && lock.release_ready !== false) {
    errors.push("diagnostic review lock must not be release-ready");
  }
  if (lock.validation_mode === "official" && lock.release_ready !== true) {
    errors.push("official release lock must be release-ready");
  }
  if (lock.validation_mode === "diagnostic_review" && lock.release_ready !== false) {
    errors.push("diagnostic review lock must not be release-ready");
  }

  if (lock.validation_mode !== "official") return;
  for (const producer of selectedProducers) {
    if (producer.kind === "sigstore_keyless_github"
        && (producer.official_ref !== "refs/heads/main"
          || producer.trigger !== "workflow_dispatch"
          || producer.require_transparency_log !== true
          || producer.require_sct !== true)) {
      errors.push(`producer ${producer.producer_id} is not eligible for official release`);
    }
    if (producer.kind === "sigstore_keyed_local"
        && (producer.transparency_log !== "none"
          || !Number.isSafeInteger(producer.key_epoch)
          || !isObject(producer.public_key))) {
      errors.push(`producer ${producer.producer_id} is not eligible for official release`);
    }
  }
}

function validateV3ReleaseLock(lock, composition, options) {
  const errors = new BoundedErrors();
  validateV3Root(lock, errors);
  const requiredTargets = validateRequiredTargets(
    options.requiredTargets, lock.release_ready, lock.validation_mode, errors,
  );
  const trustPolicy = validateV3Bindings(lock, composition, options, errors);
  const compositionById = new Map(
    composition.components.map((component) => [component.component_id, component]),
  );
  const producerById = new Map(
    trustPolicy?.producers.map((producer) => [producer.producer_id, producer]) ?? [],
  );
  const rows = Array.isArray(lock.components) ? lock.components.slice(0, V3_ROW_LIMIT) : [];
  const rowResults = rows.map((row, index) =>
    validateV3Row(row, index, compositionById, producerById, errors));
  validateV3UniquenessAndCoverage(composition, rowResults, requiredTargets, errors);
  const selectedProducers = [];
  const selectedIds = new Set();
  for (const result of rowResults) {
    if (result.producer && !selectedIds.has(result.producer.producer_id)) {
      selectedIds.add(result.producer.producer_id);
      selectedProducers.push(result.producer);
    }
  }
  validateV3Mode(lock, options, selectedProducers, errors);
  return validationResult(errors, "current_v3");
}

function validateLegacyV2ReleaseLock(lock, composition, {
  requireReady = true,
  requireNotReady = false,
  provenanceMode = "official",
  requiredTargets = undefined,
  compositionFileName = undefined,
  compositionSha256 = undefined,
} = {}) {
  const errors = [];
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
  return validationResult(errors, "legacy_v2");
}

export function validateReleaseLock(lock, compositionCandidate, options = {}) {
  let composition;
  try {
    composition = validateProductComposition(compositionCandidate);
  } catch (error) {
    return validationResult([`invalid product composition: ${error.message}`]);
  }
  if (!isObject(lock)) {
    return validationResult(["release lock schema is unsupported"]);
  }
  if (lock.schema === "arena_suite_release_lock_v3") {
    return validateV3ReleaseLock(lock, composition, {
      requireReady: options.requireReady === undefined ? true : options.requireReady,
      requireNotReady: options.requireNotReady === undefined ? false : options.requireNotReady,
      validationMode: options.validationMode,
      requiredTargets: options.requiredTargets,
      compositionFileName: options.compositionFileName,
      compositionSha256: options.compositionSha256,
      trustPolicy: options.trustPolicy,
      trustPolicyFileName: options.trustPolicyFileName,
      trustPolicySha256: options.trustPolicySha256,
    });
  }
  if (lock.schema === "arena_suite_release_lock_v2") {
    return validateLegacyV2ReleaseLock(lock, composition, options);
  }
  return validationResult(["release lock schema is unsupported"]);
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
