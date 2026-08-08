import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

import {
  loadReleaseInputs,
  validateReleaseLock,
} from "./release-lock.mjs";

const root = path.resolve(import.meta.dirname, "../..");
const composition = JSON.parse(readFileSync(
  path.join(root, "compositions", "complete-arena-0.3.9.json"),
  "utf8",
));
const draft = JSON.parse(readFileSync(
  path.join(root, "release", "catalog.lock.json"),
  "utf8",
));
const digest = "a".repeat(64);
const commit = "b".repeat(40);

function providerIdFor(lock, repository) {
  return Object.entries(lock.providers)
    .find(([, provider]) => provider.repository === repository)?.[0];
}

function providerRow(lock, component, target) {
  const providerId = providerIdFor(lock, component.repository);
  const provider = lock.providers[providerId];
  const artifactName = `${component.component_id}-${component.version}-${target}.tar.gz`;
  return {
    provider_id: providerId,
    component_id: component.component_id,
    component_version: component.version,
    target_triple: target,
    source_commit: commit,
    provider_release_tag: `component/${component.component_id}/v${component.version}/${target}`,
    artifact_name: artifactName,
    artifact_sha256: digest,
    dependencies: [...component.requires],
    provider_result_name:
      `${component.component_id}-${component.version}-${target}.provider-result.json`,
    provider_result_sha256: digest,
    provenance_bundle_name: `${artifactName}.sigstore.json`,
    provenance_bundle_sha256: digest,
    provenance_certificate_identity:
      `https://github.com/${provider.repository}/${provider.workflow_path}@refs/heads/main`,
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: provider.workflow_name,
    provenance_workflow_ref: "refs/heads/main",
    provenance_workflow_trigger: "workflow_dispatch",
  };
}

function managedRow(component, target) {
  const artifactName = `${component.component_id}-${component.version}-${target}.tar.gz`;
  return {
    component_id: component.component_id,
    component_version: component.version,
    target_triple: target,
    source_commit: commit,
    managed_release_tag: `managed/${component.component_id}/v${component.version}/${target}`,
    artifact_name: artifactName,
    artifact_sha256: digest,
    artifact_size_bytes: 100,
    installed_bytes: 80,
    dependencies: [...component.requires],
    component_manifest_sha256: digest,
    licenses_sha256: digest,
    managed_result_name:
      `${component.component_id}-${component.version}-${target}.managed-result.json`,
    managed_result_sha256: digest,
    provenance_bundle_name: `${artifactName}.sigstore.json`,
    provenance_bundle_sha256: digest,
    provenance_certificate_identity:
      "https://github.com/iteathen/uci-arena-installer/.github/workflows/managed-component-release.yml@refs/heads/main",
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: "Managed component candidate",
    provenance_workflow_ref: "refs/heads/main",
    provenance_workflow_trigger: "workflow_dispatch",
  };
}

function readyLock(product = composition, base = draft) {
  const lock = structuredClone(base);
  lock.suite_version = product.product_release;
  lock.release_ready = true;
  lock.provider_asset_mirror.release_tag = `provider-mirror/v${product.product_release}`;
  lock.components = [];
  lock.managed_components = [];
  for (const component of product.components) {
    for (const target of ["windows-x86_64", "linux-x86_64"]) {
      if (component.repository === "iteathen/uci-arena-installer") {
        lock.managed_components.push(managedRow(component, target));
      } else {
        lock.components.push(providerRow(lock, component, target));
      }
    }
  }
  return lock;
}

test("checked-in lock is exact-composition-bound draft planning data", () => {
  const inputs = loadReleaseInputs(
    path.join(root, "release", "catalog.lock.json"),
    path.join(root, "compositions"),
  );
  assert.equal(inputs.compositionSha256, draft.composition.sha256);
  assert.deepEqual(
    validateReleaseLock(inputs.lock, inputs.composition, {
      requireReady: false,
      compositionFileName: inputs.compositionFileName,
      compositionSha256: inputs.compositionSha256,
    }),
    { ok: true, errors: [] },
  );
  assert.match(
    validateReleaseLock(inputs.lock, inputs.composition).errors.join("\n"),
    /not release-ready/u,
  );
});

test("release-ready coverage and dependency identity come from composition", () => {
  const lock = readyLock();
  assert.deepEqual(validateReleaseLock(lock, composition), { ok: true, errors: [] });

  lock.components.find((row) => row.component_id === "lichess.adapter").dependencies = [];
  const result = validateReleaseLock(lock, composition);
  assert.equal(result.ok, false);
  assert.match(result.errors.join("\n"), /dependencies differ/u);
});

test("portable candidate replaces both platform rows without component special cases", () => {
  const lock = readyLock();
  const component = composition.components.find(
    (candidate) => candidate.component_id === "opening_book.standard",
  );
  lock.components = lock.components.filter(
    (row) => row.component_id !== component.component_id,
  );
  lock.components.push(providerRow(lock, component, "any-any"));
  assert.deepEqual(validateReleaseLock(lock, composition), { ok: true, errors: [] });
});

test("a new provider repository enters through composition and provider registration", () => {
  const product = structuredClone(composition);
  product.components.find((component) => component.component_id === "lichess.adapter").repository =
    "example/conforming-lichess-provider";
  const base = structuredClone(draft);
  delete base.providers.lichess_bot;
  base.providers.replacement_bot = {
    repository: "example/conforming-lichess-provider",
    workflow_path: ".github/workflows/component-release.yml",
    workflow_name: "Conforming Lichess candidate",
  };
  const lock = readyLock(product, base);
  assert.deepEqual(validateReleaseLock(lock, product), { ok: true, errors: [] });
});

test("ready lock fails when any composed component lacks platform coverage", () => {
  const lock = readyLock();
  lock.components = lock.components.filter(
    (row) => !(row.component_id === "manager.core"
      && row.target_triple === "linux-x86_64"),
  );
  const result = validateReleaseLock(lock, composition);
  assert.equal(result.ok, false);
  assert.match(result.errors.join("\n"), /manager\.core is missing linux-x86_64/u);
});

test("diagnostic review accepts exact branch provenance without granting release authority", () => {
  const lock = readyLock();
  lock.release_ready = false;
  for (const row of lock.components) {
    const provider = lock.providers[row.provider_id];
    row.provenance_workflow_ref = "refs/heads/agent/system-sanity";
    row.provenance_certificate_identity =
      `https://github.com/${provider.repository}/${provider.workflow_path}`
      + `@${row.provenance_workflow_ref}`;
  }

  assert.deepEqual(validateReleaseLock(lock, composition, {
    requireReady: false,
    requireNotReady: true,
    provenanceMode: "diagnostic_review",
    requiredTargets: ["windows-x86_64"],
  }), { ok: true, errors: [] });

  const official = validateReleaseLock(lock, composition, { requireReady: false });
  assert.equal(official.ok, false);
  assert.match(official.errors.join("\n"), /not a main-branch candidate/u);

  lock.components[0].provenance_certificate_identity += "-substituted";
  const substituted = validateReleaseLock(lock, composition, {
    requireReady: false,
    requireNotReady: true,
    provenanceMode: "diagnostic_review",
    requiredTargets: ["windows-x86_64"],
  });
  assert.equal(substituted.ok, false);
  assert.match(substituted.errors.join("\n"), /certificate_identity/u);
});
