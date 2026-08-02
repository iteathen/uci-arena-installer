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
