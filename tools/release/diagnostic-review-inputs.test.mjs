import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { buildDiagnosticReviewLock } from "./diagnostic-review-inputs.mjs";
import { validateReleaseLock } from "./release-lock.mjs";

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

const composition = {
  schema: "arena_product_composition_v1",
  product_release: "1.2.3",
  installer: { repository: "iteathen/uci-arena-installer", minimum_version: "1.0.0" },
  components: [
    {
      role: "manager", component_id: "manager.core", version: "1.0.0",
      repository: "example/manager", required: true, default_enabled: true, requires: [],
    },
    {
      role: "node_runtime", component_id: "node_runtime.private", version: "24.15.0",
      repository: "iteathen/uci-arena-installer", required: false,
      default_enabled: true, requires: [],
    },
  ],
};

const baseLock = {
  providers: {
    manager: {
      repository: "example/manager",
      workflow_path: ".github/workflows/component-release.yml",
      workflow_name: "Manager component candidate",
    },
  },
};

function writeCandidate(root, component, sourceCommit) {
  const directory = path.join(root, component.component_id);
  mkdirSync(directory);
  const artifactName = `${component.component_id}-${component.version}-windows-x86_64.tar.gz`;
  const artifactPath = path.join(directory, artifactName);
  writeFileSync(artifactPath, component.component_id);
  const managed = component.repository === "iteathen/uci-arena-installer";
  const workflowPath = managed
    ? ".github/workflows/managed-component-release.yml"
    : ".github/workflows/component-release.yml";
  const result = {
    schema: managed
      ? "arena_managed_component_release_result_v1"
      : "arena_provider_release_result_v1",
    component_id: component.component_id,
    component_version: component.version,
    target_triple: "windows-x86_64",
    source_repository: component.repository,
    source_commit: sourceCommit,
    artifact_name: artifactName,
    artifact_sha256: sha256(artifactPath),
    artifact_size_bytes: readFileSync(artifactPath).length,
    artifact_type: "archive",
    workflow_identity:
      `${component.repository}/${workflowPath}@refs/heads/agent/review#1234.1`,
  };
  if (managed) {
    Object.assign(result, {
      installed_bytes: 25,
      component_manifest_sha256: "a".repeat(64),
      licenses_sha256: "b".repeat(64),
    });
  }
  const suffix = managed ? ".managed-result.json" : ".provider-result.json";
  writeFileSync(
    path.join(directory, `${component.component_id}-${component.version}-windows-x86_64${suffix}`),
    `${JSON.stringify(result)}\n`,
  );
  writeFileSync(path.join(directory, `${artifactName}.sigstore.json`), "bundle");
  return directory;
}

test("diagnostic input builder creates complete non-promotable exact-ref lock", (t) => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-review-lock-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const commits = new Map(composition.components.map(
    (component, index) => [component.component_id, String(index + 1).repeat(40)],
  ));
  const candidateDirectories = new Map(composition.components.map((component) => [
    component.component_id,
    writeCandidate(root, component, commits.get(component.component_id)),
  ]));
  const sourceSet = {
    schema: "arena_diagnostic_review_sources_v1",
    suite_version: "1.2.3",
    target: "windows-x86_64",
    releases: composition.components.map((component) => ({
      component_id: component.component_id,
      repository: component.repository,
      release_tag: `component/${component.role}/v${component.version}/windows-x86_64`,
      source_commit: commits.get(component.component_id),
    })),
  };
  const lock = buildDiagnosticReviewLock({
    baseLock,
    composition,
    compositionFileName: "composition.json",
    compositionSha256: "c".repeat(64),
    sourceSet,
    candidateDirectories,
  });
  assert.equal(lock.release_ready, false);
  assert.equal(lock.components[0].provenance_workflow_ref, "refs/heads/agent/review");
  assert.equal(lock.managed_components[0].managed_result_sha256.length, 64);
  assert.deepEqual(validateReleaseLock(lock, composition, {
    requireReady: false,
    requireNotReady: true,
    provenanceMode: "diagnostic_review",
    requiredTargets: ["windows-x86_64"],
  }), { ok: true, errors: [] });
  assert.equal(validateReleaseLock(lock, composition).ok, false);
});

test("diagnostic input builder rejects source commit substitution", (t) => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-review-lock-reject-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const candidateDirectories = new Map(composition.components.map((component) => [
    component.component_id,
    writeCandidate(root, component, "1".repeat(40)),
  ]));
  const sourceSet = {
    schema: "arena_diagnostic_review_sources_v1",
    suite_version: "1.2.3",
    target: "windows-x86_64",
    releases: composition.components.map((component, index) => ({
      component_id: component.component_id,
      repository: component.repository,
      release_tag: "component/test/v1/windows-x86_64",
      source_commit: String(index + 2).repeat(40),
    })),
  };
  assert.throws(() => buildDiagnosticReviewLock({
    baseLock, composition, compositionFileName: "composition.json",
    compositionSha256: "c".repeat(64), sourceSet, candidateDirectories,
  }), /differs from its declared source/u);
});
