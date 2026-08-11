import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { assembleSuiteUpdatePackage } from "./build-suite-update-package.mjs";
import { sha256File } from "./release-lock.mjs";

function fixture(root) {
  const definitions = [
    { role: "manager", id: "manager.core", requires: [] },
    { role: "runtime", id: "node_runtime.private", requires: [] },
    { role: "provider", id: "lichess.adapter", requires: ["node_runtime.private"] },
  ];
  const composition = {
    schema: "arena_product_composition_v1",
    product_release: "1.2.3",
    installer: {
      repository: "iteathen/uci-arena-installer",
      minimum_version: "0.2.0",
    },
    components: definitions.map((entry) => ({
      role: entry.role,
      component_id: entry.id,
      version: "1.0.0",
      repository: entry.id === "manager.core"
        ? "iteathen/uci-arena-manager" : "iteathen/provider-fixture",
      required: entry.id !== "lichess.adapter",
      default_enabled: true,
      requires: entry.requires,
    })),
  };
  const candidates = definitions.map((entry, index) => {
    const artifactPath = path.join(root, `${entry.id}.tar.gz`);
    writeFileSync(artifactPath, `artifact-${entry.id}`);
    return {
      repository: composition.components[index].repository,
      artifactPath,
      artifactSizeBytes: readFileSync(artifactPath).length,
      manifestSizeBytes: 128,
      providerResult: { artifact_type: "archive" },
      lockRow: {
        component_id: entry.id,
        component_version: "1.0.0",
        target_triple: "windows-x86_64",
        artifact_name: path.basename(artifactPath),
        artifact_sha256: sha256File(artifactPath),
      },
      manifest: {
        schema: "arena_install_component_v1",
        schema_version: 1,
        component_id: entry.id,
        component_version: "1.0.0",
        dependencies: entry.requires,
        files: [],
      },
    };
  });
  return { composition, candidates };
}

test("routine suite update is deterministic and has no native build path", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-update-"));
  try {
    const { composition, candidates } = fixture(root);
    const applyScript = path.join(root, "apply.ps1");
    writeFileSync(applyScript, "Write-Output 'apply'\n");
    const common = {
      composition,
      candidates,
      selectedComponents: ["lichess.adapter"],
      sourceCommit: "a".repeat(40),
      sourceEpoch: 1_700_000_000,
      compositionSha256: "b".repeat(64),
      releaseLockSha256: "c".repeat(64),
      inputAuthority: "diagnostic_review_lock",
      applyScript,
    };
    const first = assembleSuiteUpdatePackage({
      ...common,
      outputRoot: path.join(root, "first"),
    });
    const second = assembleSuiteUpdatePackage({
      ...common,
      outputRoot: path.join(root, "second"),
    });
    assert.equal(first.build_path, "package_only_no_native_compilation");
    assert.equal(first.full_installer_build_required, false);
    assert.deepEqual(first.selected_components, ["lichess.adapter"]);
    assert.deepEqual(first.dependency_closure, ["lichess.adapter", "node_runtime.private"]);
    assert.equal(first.artifact.sha256, second.artifact.sha256);
    assert.equal(
      readFileSync(path.join(root, "first", "suite-update-result.json"), "utf8")
        .includes("cmake"),
      false,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("manager changes stay on the full installer path", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-update-manager-"));
  try {
    const { composition, candidates } = fixture(root);
    const applyScript = path.join(root, "apply.ps1");
    writeFileSync(applyScript, "Write-Output 'apply'\n");
    assert.throws(() => assembleSuiteUpdatePackage({
      composition,
      candidates,
      selectedComponents: ["manager.core"],
      outputRoot: path.join(root, "output"),
      sourceCommit: "a".repeat(40),
      sourceEpoch: 1_700_000_000,
      compositionSha256: "b".repeat(64),
      releaseLockSha256: "c".repeat(64),
      inputAuthority: "diagnostic_review_lock",
      applyScript,
    }), /full MSI\/installer build path/u);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
