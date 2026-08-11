import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  buildExpectedProviderStatement,
  SIGSTORE_BUNDLE_MEDIA_TYPE,
  verifyProviderCandidate,
} from "./provider-provenance.mjs";

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-provider-provenance-"));
  const provider = {
    repository: "example/provider",
    workflow_path: ".github/workflows/component-release.yml",
    workflow_name: "Example component candidate",
  };
  const artifactPath = path.join(root, "example.component-1.2.3-windows-x86_64.tar.gz");
  const resultPath = path.join(root, "example.component-1.2.3-windows-x86_64.provider-result.json");
  const bundlePath = `${artifactPath}.sigstore.json`;
  writeFileSync(artifactPath, "immutable-provider-artifact");
  const artifactSha256 = sha256(artifactPath);
  const result = {
    schema: "arena_provider_release_result_v1",
    component_id: "example.component",
    component_version: "1.2.3",
    target_triple: "windows-x86_64",
    artifact_name: path.basename(artifactPath),
    artifact_sha256: artifactSha256,
    artifact_size_bytes: readFileSync(artifactPath).length,
    component_manifest_sha256: "1".repeat(64),
    source_repository: provider.repository,
    source_commit: "2".repeat(40),
    source_tree_sha256: "3".repeat(64),
    clean_tree: true,
    release_authority: "provider_component_only",
    suite_installer_authority: "iteathen/uci-arena-installer",
    workflow_identity:
      "example/provider/.github/workflows/component-release.yml"
      + "@refs/heads/agent/review#1234.1",
    toolchain_identity: {},
    dependency_lock_identities: {},
    build_flags: [],
    sbom_sha256: "4".repeat(64),
    licenses_sha256: "5".repeat(64),
    test_receipt_sha256: "6".repeat(64),
    tests: [{ id: "test", status: "passed" }],
  };
  writeFileSync(resultPath, `${JSON.stringify(result, null, 2)}\n`);
  const statement = buildExpectedProviderStatement(result);
  const bundle = {
    mediaType: SIGSTORE_BUNDLE_MEDIA_TYPE,
    verificationMaterial: {
      certificate: { rawBytes: "certificate" },
      tlogEntries: [{ inclusionProof: { checkpoint: {} } }],
    },
    dsseEnvelope: {
      payloadType: "application/vnd.in-toto+json",
      payload: Buffer.from(JSON.stringify(statement)).toString("base64"),
      signatures: [{ sig: "signature" }],
    },
  };
  writeFileSync(bundlePath, `${JSON.stringify(bundle, null, 2)}\n`);
  const lockRow = {
    component_id: result.component_id,
    component_version: result.component_version,
    target_triple: result.target_triple,
    source_commit: result.source_commit,
    artifact_name: result.artifact_name,
    artifact_sha256: result.artifact_sha256,
    provider_result_name: path.basename(resultPath),
    provider_result_sha256: sha256(resultPath),
    provenance_bundle_name: path.basename(bundlePath),
    provenance_bundle_sha256: sha256(bundlePath),
    provenance_certificate_identity:
      "https://github.com/example/provider/.github/workflows/component-release.yml"
      + "@refs/heads/agent/review",
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: provider.workflow_name,
    provenance_workflow_ref: "refs/heads/agent/review",
    provenance_workflow_trigger: "workflow_dispatch",
  };
  return { root, provider, artifactPath, resultPath, bundlePath, lockRow, bundle };
}

test("provider verifier pins bytes, workflow claims, and exact DSSE predicate", (t) => {
  const value = fixture();
  t.after(() => rmSync(value.root, { recursive: true, force: true }));
  let observed;
  const verified = verifyProviderCandidate({
    artifactPath: value.artifactPath,
    providerResultPath: value.resultPath,
    bundlePath: value.bundlePath,
    lockRow: value.lockRow,
    provider: value.provider,
    cosignExecutable: "cosign-fixture",
    cosignVerify(executable, request) { observed = { executable, request }; },
  });
  assert.equal(verified.component_id, "example.component");
  assert.equal(observed.executable, "cosign-fixture");
  assert.equal(observed.request.sourceCommit, "2".repeat(40));
  assert.equal(observed.request.workflowRef, "refs/heads/agent/review");
});

test("provider verifier rejects a digest-valid substituted DSSE predicate", (t) => {
  const value = fixture();
  t.after(() => rmSync(value.root, { recursive: true, force: true }));
  const statement = JSON.parse(
    Buffer.from(value.bundle.dsseEnvelope.payload, "base64").toString("utf8"),
  );
  statement.predicate.source_commit = "7".repeat(40);
  value.bundle.dsseEnvelope.payload = Buffer.from(JSON.stringify(statement)).toString("base64");
  writeFileSync(value.bundlePath, `${JSON.stringify(value.bundle, null, 2)}\n`);
  value.lockRow.provenance_bundle_sha256 = sha256(value.bundlePath);
  assert.throws(() => verifyProviderCandidate({
    artifactPath: value.artifactPath,
    providerResultPath: value.resultPath,
    bundlePath: value.bundlePath,
    lockRow: value.lockRow,
    provider: value.provider,
    cosignExecutable: "cosign-fixture",
    cosignVerify() {},
  }), /does not equal the locked provider result/u);
});

test("provider verifier rejects workflow identity drift before signature acceptance", (t) => {
  const value = fixture();
  t.after(() => rmSync(value.root, { recursive: true, force: true }));
  const result = JSON.parse(readFileSync(value.resultPath, "utf8"));
  result.workflow_identity = "local-development@" + result.source_commit;
  writeFileSync(value.resultPath, `${JSON.stringify(result, null, 2)}\n`);
  value.lockRow.provider_result_sha256 = sha256(value.resultPath);
  assert.throws(() => verifyProviderCandidate({
    artifactPath: value.artifactPath,
    providerResultPath: value.resultPath,
    bundlePath: value.bundlePath,
    lockRow: value.lockRow,
    provider: value.provider,
    cosignExecutable: "cosign-fixture",
    cosignVerify() { assert.fail("Cosign must not run after identity drift"); },
  }), /workflow_identity differs/u);
});
