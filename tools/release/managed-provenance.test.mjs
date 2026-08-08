import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  MANAGED_PREDICATE_TYPE,
  SIGSTORE_BUNDLE_MEDIA_TYPE,
  buildExpectedManagedStatement,
  prepareManagedProvenance,
  verifyBundlePayload,
  verifyManagedCandidate,
} from "./managed-provenance.mjs";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../..",
);

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

test("Installer managed provenance binds exact artifact and predicate", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-installer-managed-"));
  try {
    const artifactPath = path.join(root, "node_runtime.private-24.15.0-linux-x86_64.tar.gz");
    const artifact = Buffer.from("managed artifact", "utf8");
    writeFileSync(artifactPath, artifact);
    const result = {
      schema: "arena_managed_component_release_result_v1",
      release_authority: "installer_managed_component_only",
      suite_installer_authority: "iteathen/uci-arena-installer",
      source_repository: "iteathen/uci-arena-installer",
      source_commit: "4".repeat(40),
      artifact_name: path.basename(artifactPath),
      artifact_sha256: sha256(artifact),
    };
    const resultPath = path.join(root, "managed-result.json");
    writeFileSync(resultPath, `${JSON.stringify(result)}\n`);
    const statementDirectory = path.join(root, "statements");
    const prepared = prepareManagedProvenance({
      artifactPath,
      managedResultPath: resultPath,
      statementDirectory,
    });
    assert.equal(prepared.predicateType, MANAGED_PREDICATE_TYPE);
    assert.deepEqual(prepared.statement, buildExpectedManagedStatement(result));
    const bundle = {
      mediaType: SIGSTORE_BUNDLE_MEDIA_TYPE,
      verificationMaterial: {
        certificate: { rawBytes: "certificate" },
        tlogEntries: [{ inclusionProof: { checkpoint: "checkpoint" } }],
      },
      dsseEnvelope: {
        payload: Buffer.from(JSON.stringify(prepared.statement), "utf8").toString("base64"),
        payloadType: "application/vnd.in-toto+json",
        signatures: [{ sig: "signature" }],
      },
    };
    mkdirSync(path.dirname(prepared.bundlePath), { recursive: true });
    writeFileSync(prepared.bundlePath, `${JSON.stringify(bundle)}\n`);
    assert.deepEqual(
      verifyBundlePayload(prepared.bundlePath, prepared.statementPath),
      prepared.statement,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Installer managed provenance rejects obsolete Manager authority", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-installer-managed-reject-"));
  try {
    const artifactPath = path.join(root, "artifact.tar.gz");
    const artifact = Buffer.from("managed artifact", "utf8");
    writeFileSync(artifactPath, artifact);
    const resultPath = path.join(root, "managed-result.json");
    writeFileSync(resultPath, `${JSON.stringify({
      schema: "arena_managed_component_release_result_v1",
      release_authority: "manager_managed_component_only",
      suite_installer_authority: "iteathen/uci-arena-manager",
      source_repository: "iteathen/uci-arena-manager",
      source_commit: "4".repeat(40),
      artifact_name: path.basename(artifactPath),
      artifact_sha256: sha256(artifact),
    })}\n`);
    assert.throws(() => prepareManagedProvenance({
      artifactPath,
      managedResultPath: resultPath,
      statementDirectory: path.join(root, "statements"),
    }), /does not bind the exact Installer artifact/u);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("managed candidate consumption pins lock bytes, workflow claims, and predicate", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-installer-managed-consume-"));
  try {
    const artifactPath = path.join(root, "node_runtime.private-24.15.0-windows-x86_64.tar.gz");
    const artifact = Buffer.from("managed artifact", "utf8");
    writeFileSync(artifactPath, artifact);
    const result = {
      schema: "arena_managed_component_release_result_v1",
      component_id: "node_runtime.private",
      component_version: "24.15.0",
      target_triple: "windows-x86_64",
      release_authority: "installer_managed_component_only",
      suite_installer_authority: "iteathen/uci-arena-installer",
      source_repository: "iteathen/uci-arena-installer",
      source_commit: "4".repeat(40),
      artifact_name: path.basename(artifactPath),
      artifact_sha256: sha256(artifact),
      artifact_size_bytes: artifact.length,
      component_manifest_sha256: "5".repeat(64),
      licenses_sha256: "6".repeat(64),
      workflow_identity:
        "iteathen/uci-arena-installer/.github/workflows/managed-component-release.yml"
        + "@refs/heads/main#1234.1",
    };
    const resultPath = path.join(
      root, "node_runtime.private-24.15.0-windows-x86_64.managed-result.json",
    );
    writeFileSync(resultPath, `${JSON.stringify(result)}\n`);
    const statement = buildExpectedManagedStatement(result);
    const bundlePath = `${artifactPath}.sigstore.json`;
    writeFileSync(bundlePath, `${JSON.stringify({
      mediaType: SIGSTORE_BUNDLE_MEDIA_TYPE,
      verificationMaterial: {
        certificate: { rawBytes: "certificate" },
        tlogEntries: [{ inclusionProof: { checkpoint: "checkpoint" } }],
      },
      dsseEnvelope: {
        payload: Buffer.from(JSON.stringify(statement), "utf8").toString("base64"),
        payloadType: "application/vnd.in-toto+json",
        signatures: [{ sig: "signature" }],
      },
    })}\n`);
    const lockRow = {
      component_id: result.component_id,
      component_version: result.component_version,
      target_triple: result.target_triple,
      source_commit: result.source_commit,
      artifact_name: result.artifact_name,
      artifact_sha256: result.artifact_sha256,
      component_manifest_sha256: result.component_manifest_sha256,
      licenses_sha256: result.licenses_sha256,
      managed_result_name: path.basename(resultPath),
      managed_result_sha256: sha256(readFileSync(resultPath)),
      provenance_bundle_name: path.basename(bundlePath),
      provenance_bundle_sha256: sha256(readFileSync(bundlePath)),
      provenance_certificate_identity:
        "https://github.com/iteathen/uci-arena-installer/"
        + ".github/workflows/managed-component-release.yml@refs/heads/main",
      provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
      provenance_workflow_name: "Managed component candidate",
      provenance_workflow_ref: "refs/heads/main",
      provenance_workflow_trigger: "workflow_dispatch",
    };
    let request;
    const verified = verifyManagedCandidate({
      artifactPath,
      managedResultPath: resultPath,
      bundlePath,
      lockRow,
      cosignExecutable: "cosign-fixture",
      cosignVerify(executable, value) { request = { executable, ...value }; },
    });
    assert.equal(verified.component_id, "node_runtime.private");
    assert.equal(request.executable, "cosign-fixture");
    assert.equal(request.sourceCommit, "4".repeat(40));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("managed candidates are manual, private, keyless, and Installer-owned", () => {
  const workflow = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "managed-component-release.yml"),
    "utf8",
  );
  assert.match(workflow, /workflow_dispatch:/u);
  assert.doesNotMatch(workflow, /^\s*(?:push|pull_request|schedule):/mu);
  assert.match(workflow, /id-token:\s*write/u);
  assert.match(workflow, /managed-provenance\.mjs prepare/u);
  assert.match(workflow, /uci-arena-installer retains|Installer-managed/u);
  assert.doesNotMatch(workflow, /uci-arena-manager\\tools|uci-arena-manager\/tools/u);
});
