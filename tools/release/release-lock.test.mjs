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

const v3Composition = {
  schema: "arena_product_composition_v1",
  product_release: "1.2.3",
  installer: {
    repository: "iteathen/uci-arena-installer",
    minimum_version: "1.2.3",
  },
  components: [
    {
      role: "engine",
      component_id: "engine.core",
      version: "1.2.3",
      repository: "example/engine",
      required: true,
      default_enabled: true,
      requires: [],
    },
    {
      role: "helper",
      component_id: "helper.library",
      version: "1.2.3",
      repository: "example/engine",
      required: true,
      default_enabled: true,
      requires: [],
    },
    {
      role: "bootstrap",
      component_id: "bootstrap.runtime",
      version: "1.2.3",
      repository: "iteathen/uci-arena-installer",
      required: true,
      default_enabled: true,
      requires: ["engine.core", "helper.library"],
    },
  ],
};

function v3TrustPolicy() {
  return {
    schema: "arena_provenance_trust_policy_v1",
    schema_version: 1,
    policy_id: "suite-release",
    policy_version: 1,
    suite_installer_authority: "iteathen/uci-arena-installer",
    cosign_version: "3.0.6",
    producers: [
      {
        producer_id: "github-engine",
        kind: "sigstore_keyless_github",
        key_epoch: 1,
        evidence_format: "provider_result_v1",
        scopes: [{
          source_repository: "example/engine",
          ownership: "provider",
          component_ids: ["engine.core", "helper.library"],
        }],
        predicate_type:
          "https://uci-arena.example/attestations/provider-release-result/v1",
        oidc_issuer: "https://token.actions.githubusercontent.com",
        workflow_path: ".github/workflows/component-release.yml",
        workflow_name: "Engine component candidate",
        official_ref: "refs/heads/main",
        trigger: "workflow_dispatch",
        allow_diagnostic_refs: true,
        require_transparency_log: true,
        require_sct: true,
      },
      {
        producer_id: "local-bootstrap",
        kind: "sigstore_keyed_local",
        key_epoch: 2,
        evidence_format: "component_evidence_v1",
        scopes: [{
          source_repository: "iteathen/uci-arena-installer",
          ownership: "installer_managed",
          component_ids: ["bootstrap.runtime"],
        }],
        predicate_type:
          "https://uci-arena.example/attestations/component-release-evidence/v1",
        public_key: {
          path: "keys/release.pub",
          sha256: "c".repeat(64),
          spki_sha256: "d".repeat(64),
        },
        transparency_log: "none",
      },
    ],
  };
}

function v3Row(component, target) {
  const managed = component.repository === "iteathen/uci-arena-installer";
  const prefix = `${component.component_id}-${target}`;
  return {
    component_id: component.component_id,
    component_version: component.version,
    target_triple: target,
    ownership: managed ? "installer_managed" : "provider",
    source_repository: component.repository,
    source_commit: commit,
    artifact: {
      name: `${prefix}.tar.gz`,
      sha256: digest,
      size_bytes: 100,
      type: "archive",
    },
    component_manifest_sha256: digest,
    dependencies: [...component.requires],
    evidence: {
      format: managed ? "component_evidence_v1" : "provider_result_v1",
      name: `${prefix}.evidence.json`,
      sha256: digest,
    },
    provenance: {
      producer_id: managed ? "local-bootstrap" : "github-engine",
      bundle_name: `${prefix}.sigstore.json`,
      bundle_sha256: digest,
    },
  };
}

function v3Lock(targets = ["windows-x86_64", "linux-x86_64"]) {
  return {
    schema: "arena_suite_release_lock_v3",
    suite_version: v3Composition.product_release,
    release_ready: true,
    validation_mode: "official",
    composition: {
      manifest_name: "complete-arena-1.2.3.json",
      sha256: "e".repeat(64),
    },
    trust_policy: {
      name: "release-policy.json",
      sha256: "f".repeat(64),
    },
    components: v3Composition.components.flatMap((component) =>
      targets.map((target) => v3Row(component, target))),
  };
}

function v3Options(overrides = {}) {
  return {
    requireReady: true,
    requireNotReady: false,
    validationMode: "official",
    requiredTargets: ["windows-x86_64", "linux-x86_64"],
    compositionFileName: "complete-arena-1.2.3.json",
    compositionSha256: "e".repeat(64),
    trustPolicy: v3TrustPolicy(),
    trustPolicyFileName: "release-policy.json",
    trustPolicySha256: "f".repeat(64),
    ...overrides,
  };
}

function invalidV3(lock, options, pattern) {
  const result = validateReleaseLock(lock, v3Composition, options);
  assert.equal(result.ok, false);
  assert.match(result.errors.join("\n"), pattern);
  return result;
}

test("v3 validates exact official rows and reports format compatibility", () => {
  const result = validateReleaseLock(v3Lock(), v3Composition, v3Options());
  assert.deepEqual(result, { ok: true, errors: [] });
  assert.equal(result.compatibility, "current_v3");

  const legacy = validateReleaseLock(draft, composition, { requireReady: false });
  assert.equal(legacy.ok, true);
  assert.equal(legacy.compatibility, "legacy_v2");
  assert.match(
    validateReleaseLock({ schema: "arena_suite_release_lock_v1" }, composition).errors[0],
    /unsupported/u,
  );
});

test("v3 portable rows cover both targets without platform special cases", () => {
  const result = validateReleaseLock(v3Lock(["any"]), v3Composition, v3Options());
  assert.deepEqual(result, { ok: true, errors: [] });
});

test("v3 rejects duplicate row identities and portable/platform overlap", () => {
  const duplicate = v3Lock();
  duplicate.components.push(structuredClone(duplicate.components[0]));
  invalidV3(duplicate, v3Options(), /candidate identity is duplicated/u);

  const overlap = v3Lock();
  overlap.components.push(v3Row(v3Composition.components[0], "any"));
  invalidV3(overlap, v3Options(), /mixes portable and platform candidates/u);
});

test("v3 requires every component and target while allowing an intentional superset", () => {
  const missingTarget = v3Lock();
  missingTarget.components = missingTarget.components.filter((row) =>
    !(row.component_id === "engine.core" && row.target_triple === "linux-x86_64"));
  invalidV3(missingTarget, v3Options(), /engine\.core is missing linux-x86_64/u);

  const missingComponent = v3Lock();
  missingComponent.components = missingComponent.components.filter(
    (row) => row.component_id !== "bootstrap.runtime",
  );
  invalidV3(missingComponent, v3Options(), /bootstrap\.runtime has no candidate row/u);

  const superset = validateReleaseLock(v3Lock(), v3Composition, v3Options({
    requiredTargets: ["windows-x86_64"],
  }));
  assert.deepEqual(superset, { ok: true, errors: [] });
});

test("v3 binds every row to composition identity and order-independent dependencies", () => {
  const reordered = v3Lock();
  const bootstrap = reordered.components.find(
    (row) => row.component_id === "bootstrap.runtime",
  );
  bootstrap.dependencies = [...bootstrap.dependencies].reverse();
  assert.equal(validateReleaseLock(reordered, v3Composition, v3Options()).ok, true);

  for (const mutate of [
    (row) => { row.component_version = "9.9.9"; },
    (row) => { row.source_repository = "example/substituted"; },
    (row) => { row.ownership = "installer_managed"; },
    (row) => { row.dependencies = ["bootstrap.runtime"]; },
  ]) {
    const lock = v3Lock();
    mutate(lock.components[0]);
    invalidV3(lock, v3Options(), /differ(?:s)? from the product composition/u);
  }

  const undeclared = v3Lock();
  undeclared.components.push({
    ...v3Row(v3Composition.components[0], "any"),
    component_id: "undeclared.component",
  });
  invalidV3(undeclared, v3Options(), /not declared by the product composition/u);
});

test("v3 enforces exact bounded artifact, evidence, and provenance identity", () => {
  const mutations = [
    [(row) => { row.artifact.name = "unsafe/path.tar.gz"; }, /artifact\.name is invalid/u],
    [(row) => { row.artifact.sha256 = "A".repeat(64); }, /artifact\.sha256 is invalid/u],
    [(row) => { row.artifact.size_bytes = 1_099_511_627_777; }, /size_bytes is invalid/u],
    [(row) => { row.artifact.type = "file_set"; }, /file_count is required/u],
    [(row) => { row.artifact.file_count = 1; }, /must not contain file-set metrics/u],
    [(row) => { row.evidence.name = row.artifact.name; }, /asset name is duplicated globally/u],
    [(row) => { row.evidence.name = "unsafe/evidence.json"; }, /evidence\.name is invalid/u],
    [(row) => { row.evidence.sha256 = "A".repeat(64); }, /evidence\.sha256 is invalid/u],
    [(row) => { row.provenance.bundle_name = "unsafe/bundle.json"; },
      /bundle_name is invalid/u],
    [(row) => { row.provenance.bundle_sha256 = "bad"; }, /bundle_sha256 is invalid/u],
  ];
  for (const [mutate, pattern] of mutations) {
    const lock = v3Lock();
    mutate(lock.components[0]);
    invalidV3(lock, v3Options(), pattern);
  }

  const validFileSet = v3Lock();
  validFileSet.components[0].artifact = {
    ...validFileSet.components[0].artifact,
    type: "file_set",
    file_count: 3,
    installed_bytes: 90,
  };
  assert.equal(validateReleaseLock(validFileSet, v3Composition, v3Options()).ok, true);

  const crossRowCollision = v3Lock();
  crossRowCollision.components[1].artifact.name = crossRowCollision.components[0].artifact.name;
  invalidV3(crossRowCollision, v3Options(), /asset name is duplicated globally/u);
});

test("v3 requires exact composition and trust-policy bindings", () => {
  for (const [mutateLock, mutateOptions, pattern] of [
    [(lock) => { lock.composition.manifest_name = "complete-arena-1.2.4.json"; },
      () => {}, /manifest_name/u],
    [(lock) => { lock.composition.sha256 = "0".repeat(64); },
      () => {}, /composition sha256 differs/u],
    [(lock) => { lock.trust_policy.name = "other-policy.json"; },
      () => {}, /trust_policy name differs/u],
    [(lock) => { lock.trust_policy.sha256 = "0".repeat(64); },
      () => {}, /trust_policy sha256 differs/u],
    [() => {}, (options) => { options.compositionFileName = "complete-arena-1.2.4.json"; },
      /composition manifest_name differs/u],
    [() => {}, (options) => { options.trustPolicyFileName = "other-policy.json"; },
      /trust_policy name differs/u],
  ]) {
    const lock = v3Lock();
    const options = v3Options();
    mutateLock(lock);
    mutateOptions(options);
    invalidV3(lock, options, pattern);
  }

  invalidV3(v3Lock(), v3Options({ trustPolicy: undefined }), /invalid provenance trust policy/u);
});

test("v3 binds explicit producers to exact scopes, formats, and kinds", () => {
  const unknown = v3Lock();
  unknown.components[0].provenance.producer_id = "unknown";
  invalidV3(unknown, v3Options(), /producer_id is unknown/u);

  const scopeOptions = v3Options();
  scopeOptions.trustPolicy.producers[0].scopes[0].source_repository = "example/other";
  invalidV3(v3Lock(), scopeOptions, /outside the selected producer scope/u);

  const format = v3Lock();
  format.components[0].evidence.format = "component_evidence_v1";
  invalidV3(format, v3Options(), /differs from the selected producer/u);

  const invalidKindPolicy = v3TrustPolicy();
  invalidKindPolicy.producers[1].evidence_format = "provider_result_v1";
  invalidV3(v3Lock(), v3Options({ trustPolicy: invalidKindPolicy }), /local producer/u);
});

test("v3 exact fields exclude transport and producer-specific authority", () => {
  const transport = v3Lock();
  transport.provider_asset_mirror = { repository: "example/mirror" };
  invalidV3(transport, v3Options(), /root fields are invalid/u);

  const producerSpecific = v3Lock();
  producerSpecific.components[0].provenance_workflow_ref = "refs/heads/main";
  invalidV3(producerSpecific, v3Options(), /components\[0\] fields are invalid/u);

  const localKey = v3Lock();
  localKey.components.find(
    (row) => row.ownership === "installer_managed",
  ).provenance.public_key = "keys/release.pub";
  invalidV3(localKey, v3Options(), /provenance fields are invalid/u);
});

test("v3 official and diagnostic modes remain contradictory by construction", () => {
  const diagnostic = v3Lock();
  diagnostic.release_ready = false;
  diagnostic.validation_mode = "diagnostic_review";
  assert.deepEqual(validateReleaseLock(diagnostic, v3Composition, v3Options({
    requireReady: false,
    requireNotReady: true,
    validationMode: "diagnostic_review",
  })), { ok: true, errors: [] });

  const draftOfficial = v3Lock();
  draftOfficial.release_ready = false;
  invalidV3(draftOfficial, v3Options({ requireReady: false }), /official release lock/u);

  const readyDiagnostic = v3Lock();
  readyDiagnostic.validation_mode = "diagnostic_review";
  invalidV3(readyDiagnostic, v3Options({
    validationMode: "diagnostic_review",
  }), /diagnostic review lock must not be release-ready/u);

  invalidV3(v3Lock(), v3Options({ validationMode: "diagnostic_review" }),
    /differs from the release lock/u);
});

test("v3 validates required targets and rejects legacy portable vocabulary", () => {
  invalidV3(v3Lock(), v3Options({ requiredTargets: [] }), /must be nonempty/u);
  invalidV3(v3Lock(), v3Options({ requiredTargets: ["any"] }), /supported platform/u);
  const legacyPortable = v3Lock(["any"]);
  for (const row of legacyPortable.components) row.target_triple = "any-any";
  invalidV3(legacyPortable, v3Options(), /target_triple is invalid/u);
});

test("v3 hostile input diagnostics are bounded and deterministic", () => {
  const hostile = v3Lock();
  hostile.components = Array.from({ length: 600 }, () => ({}));
  const first = validateReleaseLock(hostile, v3Composition, v3Options());
  const second = validateReleaseLock(hostile, v3Composition, v3Options());
  assert.equal(first.ok, false);
  assert(first.errors.length <= 64);
  assert.equal(first.errors.at(-1), "additional validation errors omitted");
  assert.deepEqual(first.errors, second.errors);
});
