import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
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
  verifyGitHubManagedCandidate,
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
        signatures: [{ sig: Buffer.from("signature").toString("base64") }],
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
        signatures: [{ sig: Buffer.from("signature").toString("base64") }],
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

for (const [component, target, expected] of [
  ["node_runtime.private", "windows-x86_64", {
    component_version: "26.11.1", input_name: "node-v26.11.1-win-x64.zip",
    source_sha256: "97f36a8a9684ff0d3e35758b4610fef5b628a5880e96f2ccc11240e5daf9934e",
    source_url: "https://nodejs.org/download/release/v26.11.1/node-v26.11.1-win-x64.zip",
    network_name: "", network_sha256: "", network_url: "", source_offer_url: "", tag_slug: "node-runtime",
  }],
  ["node_runtime.private", "linux-x86_64", {
    component_version: "26.11.1", input_name: "node-v26.11.1-linux-x64.tar.gz",
    source_sha256: "15fc043a51a5783f2f4b634561091658f5a7f0e3db03f997dca3a8033a33e45b",
    source_url: "https://nodejs.org/download/release/v26.11.1/node-v26.11.1-linux-x64.tar.gz",
    network_name: "", network_sha256: "", network_url: "", source_offer_url: "", tag_slug: "node-runtime",
  }],
  ...["windows-x86_64", "linux-x86_64"].map(target => ["stockfish.engine", target, {
    component_version: "15.1", input_name: "stockfish-15.1-source.tar.gz",
    source_sha256: "d4272657905319328294355973faee40a8c28e3eecb0e7b266ed34ff33383b76",
    source_url: "https://github.com/official-stockfish/Stockfish/archive/refs/tags/sf_15.1.tar.gz",
    network_name: "nn-ad9b42354671.nnue",
    network_sha256: "ad9b423546714137916bd38978af6fd68d7b8951bef25ff76bf43da72d6cb786",
    network_url: "https://tests.stockfishchess.org/api/nn/nn-ad9b42354671.nnue",
    source_offer_url: "https://github.com/official-stockfish/Stockfish/archive/refs/tags/sf_15.1.tar.gz",
    tag_slug: "stockfish",
  }]),
]) {
  test(`managed workflow resolves exact pinned ${component} input for ${target}`, () => {
    const root = mkdtempSync(path.join(os.tmpdir(), "arena-managed-input-resolution-"));
    try {
      const workflow = readFileSync(path.join(repositoryRoot, ".github/workflows/managed-component-release.yml"), "utf8");
      const step = workflow.split("      - name: Resolve exact upstream input")[1]
        ?.split("      - name: Download and verify exact upstream input")[0];
      const run = /        run: \|\r?\n([\s\S]*)/u.exec(step ?? "")?.[1];
      assert.equal(typeof run, "string", "the actual upstream resolution step must exist");
      const script = run.split(/\r?\n/u).map(line => line.replace(/^          /u, "")).join("\n")
        .replaceAll("${{ inputs.component_id }}", component).replaceAll("${{ inputs.target }}", target);
      const scriptPath = path.join(root, "resolve-input.ps1");
      const outputPath = path.join(root, "github-output.txt");
      writeFileSync(scriptPath, script);
      execFileSync(process.platform === "win32" ? "pwsh.exe" : "pwsh", [
        "-NoProfile", "-NonInteractive", "-File", scriptPath,
      ], { windowsHide: true, timeout: 15_000, env: { ...process.env, GITHUB_OUTPUT: outputPath } });
      const actual = Object.fromEntries(readFileSync(outputPath, "utf8").replace(/^\uFEFF/u, "").trim()
        .split(/\r?\n/u).map(line => { const separator = line.indexOf("="); return [line.slice(0, separator), line.slice(separator + 1)]; }));
      assert.deepEqual(actual, expected);
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test("GitHub managed adapter returns exact normalized provenance",()=>{const root=mkdtempSync(path.join(os.tmpdir(),"managed-adapter-"));try{const artifactPath=path.join(root,"node.tar");const artifact=Buffer.from("artifact");writeFileSync(artifactPath,artifact);const result={schema:"arena_managed_component_release_result_v1",component_id:"node_runtime.private",component_version:"24.15.0",target_triple:"any-any",release_authority:"installer_managed_component_only",suite_installer_authority:"iteathen/uci-arena-installer",source_repository:"iteathen/uci-arena-installer",source_commit:"4".repeat(40),artifact_name:"node.tar",artifact_sha256:sha256(artifact),artifact_size_bytes:artifact.length,installed_bytes:artifact.length,component_manifest_sha256:"5".repeat(64),licenses_sha256:"8".repeat(64),upstream_inputs:[{role:"source",url:"https://example.invalid/node.tar",sha256:"9".repeat(64)}],validation_evidence:{executable_smoke:{status:"passed"}},workflow_identity:"iteathen/uci-arena-installer/.github/workflows/managed-component-release.yml@refs/heads/main#2.1"};const statement=buildExpectedManagedStatement(result);const bundle=Buffer.from(JSON.stringify({mediaType:SIGSTORE_BUNDLE_MEDIA_TYPE,verificationMaterial:{certificate:{rawBytes:"certificate"},tlogEntries:[{inclusionProof:{checkpoint:{}}}]},dsseEnvelope:{payloadType:"application/vnd.in-toto+json",payload:Buffer.from(JSON.stringify(statement)).toString("base64"),signatures:[{sig:Buffer.from("signature").toString("base64")}]}}));const row={component_id:result.component_id,component_version:result.component_version,target_triple:"any",ownership:"installer_managed",source_repository:result.source_repository,source_commit:result.source_commit,artifact:{name:"node.tar",sha256:result.artifact_sha256,size_bytes:artifact.length,type:"archive"},component_manifest_sha256:result.component_manifest_sha256,dependencies:[],evidence:{format:"managed_result_v1",name:"result.json",sha256:"6".repeat(64)},provenance:{producer_id:"managed",bundle_name:"bundle.json",bundle_sha256:"7".repeat(64)}};const producer={producer_id:"managed",kind:"sigstore_keyless_github",key_epoch:2,evidence_format:"managed_result_v1",workflow_path:".github/workflows/managed-component-release.yml",workflow_name:"Managed component candidate",official_ref:"refs/heads/main",allow_diagnostic_refs:false,oidc_issuer:"https://token.actions.githubusercontent.com",trigger:"workflow_dispatch"};const verified=verifyGitHubManagedCandidate({assets:{artifact:{path:artifactPath,sizeBytes:artifact.length},evidence:{json:result},bundle:{path:path.join(root,"bundle.json"),bytes:bundle}},row,component:{},producer,cosignPath:"fixture",cosignVerify(){}});assert.equal(verified.target_triple,"any");assert.equal(verified.ownership,"installer_managed");assert.equal(verified.dependencies_verified_by,"release_lock");}finally{rmSync(root,{recursive:true,force:true});}});
