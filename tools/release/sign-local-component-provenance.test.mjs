import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash, createPublicKey, generateKeyPairSync, randomBytes } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { runCosign } from "./cosign-cli.mjs";
import {
  buildOfflineEnvironment,
  parseArguments,
  signLocalComponentProvenance,
} from "./sign-local-component-provenance.mjs";

const sha = "a".repeat(64);
const commit = "b".repeat(40);
const repositoryRoot = path.resolve(import.meta.dirname, "../..");

function digest(value) {
  return createHash("sha256").update(value).digest("hex");
}

function temporaryRoot(prefix) {
  return mkdtempSync(path.join(os.tmpdir(), prefix));
}

function evidenceFor(artifactBytes) {
  return {
    schema: "arena_component_release_evidence_v1",
    schema_version: 1,
    component_id: "engine.core",
    component_version: "1.2.3",
    target_triple: "linux-x86_64",
    ownership: "provider",
    source: {
      repository: "example/engine",
      commit,
      tree_sha256: sha,
      clean_tree: true,
    },
    artifact: {
      name: "engine.tar",
      sha256: digest(artifactBytes),
      size_bytes: artifactBytes.byteLength,
      type: "archive",
    },
    component_manifest_sha256: sha,
    dependencies: ["runtime.core"],
    build: {
      builder_id: "offline-builder",
      invocation_id: "canary-evidence-claim",
      toolchain_identity_sha256: sha,
      tests_passed: true,
      materials: [{
        role: "source",
        uri: "git+https://github.com/example/engine",
        sha256: sha,
      }],
      evidence_assets: [
        { kind: "licenses", name: "licenses.json", sha256: sha, size_bytes: 1 },
        { kind: "test_receipt", name: "tests.json", sha256: sha, size_bytes: 1 },
      ],
    },
  };
}

function policyFor(publicKeyBytes, publicKeyObject) {
  return {
    schema: "arena_provenance_trust_policy_v1",
    schema_version: 1,
    policy_id: "offline-release",
    policy_version: 1,
    suite_installer_authority: "iteathen/uci-arena-installer",
    cosign_version: "3.0.6",
    producers: [{
      producer_id: "local-engine",
      kind: "sigstore_keyed_local",
      key_epoch: 4,
      evidence_format: "component_evidence_v1",
      scopes: [{
        source_repository: "example/engine",
        ownership: "provider",
        component_ids: ["engine.core"],
      }],
      predicate_type:
        "https://uci-arena.example/attestations/component-release-evidence/v1",
      public_key: {
        path: "keys/release.pub",
        sha256: digest(publicKeyBytes),
        spki_sha256: digest(publicKeyObject.export({ type: "spki", format: "der" })),
      },
      transparency_log: "none",
    }],
  };
}

function fixture({ publicKeyBytes, publicKeyObject, signingKeyPath } = {}) {
  const roots = [
    temporaryRoot("rp04-candidate-"),
    temporaryRoot("rp04-policy-"),
    temporaryRoot("rp04-key-"),
    temporaryRoot("rp04-output-"),
    temporaryRoot("rp04-tool-"),
  ];
  const [candidateRoot, policyRoot, keyRoot, outputRoot, toolRoot] = roots;
  mkdirSync(path.join(policyRoot, "keys"));
  const pair = publicKeyObject
    ? { publicKey: publicKeyObject }
    : generateKeyPairSync("ed25519");
  const keyBytes = publicKeyBytes
    ?? pair.publicKey.export({ type: "spki", format: "pem" });
  const publicKeyPath = path.join(policyRoot, "keys", "release.pub");
  writeFileSync(publicKeyPath, keyBytes);
  const actualSigningKey = signingKeyPath ?? path.join(keyRoot, "release.key");
  if (signingKeyPath === undefined) writeFileSync(actualSigningKey, "opaque-private-key-canary");
  const artifactBytes = Buffer.from("exact artifact bytes");
  const artifactPath = path.join(candidateRoot, "engine.tar");
  writeFileSync(artifactPath, artifactBytes);
  const evidence = evidenceFor(artifactBytes);
  const evidencePath = path.join(candidateRoot, "component-evidence.json");
  writeFileSync(evidencePath, JSON.stringify(evidence));
  const policy = policyFor(keyBytes, pair.publicKey);
  const policyPath = path.join(policyRoot, "trust-policy.json");
  writeFileSync(policyPath, JSON.stringify(policy));
  const cosignPath = path.join(toolRoot, process.platform === "win32" ? "cosign.exe" : "cosign");
  writeFileSync(cosignPath, "fake cosign");
  const outputDirectory = path.join(outputRoot, "signed-candidate");
  return {
    roots,
    candidateRoot,
    policyRoot,
    keyRoot,
    outputRoot,
    artifactBytes,
    artifactPath,
    evidence,
    evidencePath,
    policy,
    policyPath,
    publicKeyPath,
    signingKeyPath: actualSigningKey,
    cosignPath,
    outputDirectory,
    cleanup() {
      for (const root of roots) rmSync(root, { recursive: true, force: true });
    },
    rewriteEvidence() {
      writeFileSync(evidencePath, JSON.stringify(evidence));
    },
    rewritePolicy() {
      writeFileSync(policyPath, JSON.stringify(policy));
    },
    options(overrides = {}) {
      return {
        artifact: artifactPath,
        evidence: evidencePath,
        trustPolicy: policyPath,
        producerId: "local-engine",
        signingKey: actualSigningKey,
        outputDirectory,
        cosign: cosignPath,
        env: {
          ...process.env,
          COSIGN_PASSWORD: "canary-password",
          COSIGN_EXPERIMENTAL: "canary-cosign-env",
          GITHUB_TOKEN: "canary-github-token",
          ACTIONS_ID_TOKEN_REQUEST_TOKEN: "canary-oidc-token",
          SIGSTORE_ID_TOKEN: "canary-sigstore-token",
          HTTPS_PROXY: "http://127.0.0.1:1",
        },
        ...overrides,
      };
    },
  };
}

function verifiedFromRequest(request) {
  return {
    schema: "arena_verified_component_provenance_v1",
    component_id: request.row.component_id,
    component_version: request.row.component_version,
    target_triple: request.row.target_triple,
    ownership: request.row.ownership,
    source_repository: request.row.source_repository,
    source_commit: request.row.source_commit,
    artifact: structuredClone(request.row.artifact),
    component_manifest_sha256: request.row.component_manifest_sha256,
    dependencies: [...request.row.dependencies],
    dependencies_verified_by: "signed_evidence",
    producer: {
      producer_id: request.producer.producer_id,
      kind: request.producer.kind,
      key_epoch: request.producer.key_epoch,
      audit_level: "self_managed_key",
    },
    evidence: structuredClone(request.row.evidence),
    bundle: {
      name: request.row.provenance.bundle_name,
      sha256: request.row.provenance.bundle_sha256,
    },
  };
}

function fakeCosign(calls, { failAfterWrite = false, inspectStatement } = {}) {
  return (request) => {
    calls.push(request);
    const bundlePath = request.args[request.args.indexOf("--bundle") + 1];
    const statementPath = request.args[request.args.indexOf("--statement") + 1];
    if (inspectStatement) inspectStatement(JSON.parse(readFileSync(statementPath, "utf8")));
    writeFileSync(bundlePath, JSON.stringify({ fake: "bundle" }));
    if (failAfterWrite) throw new Error("Cosign command failed (ETIMEDOUT)");
    return Object.freeze({ status: 0, stdout: "", stderr: "" });
  };
}

function stagingEntries(item) {
  return readdirSync(item.outputRoot)
    .filter((name) => name.startsWith(".signed-candidate.staging-"));
}

test("local signer parses only the exact explicit CLI", () => {
  const argv = [
    "--artifact", "artifact",
    "--evidence", "evidence",
    "--trust-policy", "policy",
    "--producer-id", "producer",
    "--signing-key", "key",
    "--output-directory", "output",
    "--cosign", "cosign",
  ];
  assert.deepEqual(parseArguments(argv), {
    artifact: "artifact",
    evidence: "evidence",
    trustPolicy: "policy",
    producerId: "producer",
    signingKey: "key",
    outputDirectory: "output",
    cosign: "cosign",
  });
  for (const invalid of [
    argv.slice(0, -4),
    [...argv, "--unknown", "canary-argument"],
    [...argv, "--artifact", "duplicate"],
    argv.slice(0, -1),
    argv.map((value, index) => index === 1 ? "--evidence" : value),
  ]) {
    assert.throws(() => parseArguments(invalid), (error) => {
      assert.doesNotMatch(error.message, /canary-argument|duplicate/u);
      return true;
    });
  }

  const cli = spawnSync(process.execPath, [
    path.join(import.meta.dirname, "sign-local-component-provenance.mjs"),
    "--unknown",
    "canary-cli-secret",
  ], { encoding: "utf8", shell: false, windowsHide: true });
  assert.notEqual(cli.status, 0);
  assert.match(cli.stderr, /arguments are invalid/u);
  assert.doesNotMatch(`${cli.stdout}${cli.stderr}`, /canary-cli-secret/u);
});

test("local signer publishes one verified two-file output with exact offline invocation", async () => {
  const item = fixture();
  try {
    const calls = [];
    let signedStatement;
    let verificationCount = 0;
    const result = await signLocalComponentProvenance(item.options(), {
      cosignRun: fakeCosign(calls, {
        inspectStatement(statement) { signedStatement = statement; },
      }),
      verifyCandidate(request) {
        verificationCount += 1;
        assert.equal(request.assets.artifact.path, item.artifactPath);
        assert.equal(path.dirname(request.assets.evidence.path).startsWith(item.outputRoot), true);
        return verifiedFromRequest(request);
      },
    });
    assert.equal(calls.length, 1);
    assert.equal(verificationCount, 1);
    const call = calls[0];
    assert.equal(call.cosignPath, item.cosignPath);
    assert.deepEqual(call.args.slice(0, 2), ["attest-blob", "--statement"]);
    assert.equal(call.args[3], "--key");
    assert.equal(call.args[4], item.signingKeyPath);
    assert.equal(call.args[5], "--bundle");
    assert.deepEqual(call.args.slice(7), [
      "--tlog-upload=false",
      "--use-signing-config=false",
      "--yes",
      item.artifactPath,
    ]);
    assert.equal(call.timeoutMs, 180_000);
    assert.equal(call.maxOutputBytes, 1024 * 1024);
    assert.equal(call.env.COSIGN_PASSWORD, "canary-password");
    assert.equal(call.env.GITHUB_TOKEN, undefined);
    assert.equal(call.env.ACTIONS_ID_TOKEN_REQUEST_TOKEN, undefined);
    assert.equal(call.env.SIGSTORE_ID_TOKEN, undefined);
    assert.equal(call.env.COSIGN_EXPERIMENTAL, undefined);
    assert.equal(call.env.HTTPS_PROXY, "http://127.0.0.1:1");
    assert.deepEqual(signedStatement.predicate, item.evidence);

    const expectedFiles = ["component-evidence.json", "engine.tar.sigstore.json"];
    assert.deepEqual(readdirSync(item.outputDirectory).sort(), expectedFiles);
    assert.deepEqual(
      readFileSync(path.join(item.outputDirectory, "component-evidence.json")),
      readFileSync(item.evidencePath),
    );
    assert.deepEqual(Object.keys(result), [
      "schema", "state", "component_id", "component_version", "target_triple",
      "artifact_name", "artifact_sha256", "evidence_name", "evidence_sha256",
      "bundle_name", "bundle_sha256", "producer_id", "key_epoch", "audit_level",
      "output_directory",
    ]);
    const rendered = JSON.stringify(result);
    assert.doesNotMatch(rendered, /canary|release\.key|release\.pub|trust-policy/u);
    assert.equal(result.output_directory, item.outputDirectory);
    assert.deepEqual(stagingEntries(item), []);
  } finally {
    item.cleanup();
  }
});

test("local signer rejects every artifact identity mismatch before Cosign", async () => {
  const mutations = [
    (item) => { item.evidence.artifact.name = "other.tar"; },
    (item) => { item.evidence.artifact.sha256 = "0".repeat(64); },
    (item) => { item.evidence.artifact.size_bytes += 1; },
    (item) => { item.evidence.artifact.type = "file_set"; },
  ];
  for (const mutate of mutations) {
    const item = fixture();
    try {
      mutate(item);
      item.rewriteEvidence();
      let calls = 0;
      await assert.rejects(signLocalComponentProvenance(item.options(), {
        cosignRun() { calls += 1; },
      }), /artifact differs|component evidence|component provenance/u);
      assert.equal(calls, 0);
      assert.equal(existsSync(item.outputDirectory), false);
    } finally {
      item.cleanup();
    }
  }
});

test("local signer binds canonical evidence to one exact local producer and public key", async () => {
  const missingProducer = fixture();
  try {
    await assert.rejects(signLocalComponentProvenance(missingProducer.options({
      producerId: "missing",
    }), {
      cosignRun() { assert.fail("Cosign must not run"); },
    }), /unknown producer/u);
  } finally {
    missingProducer.cleanup();
  }

  const cases = [
    [(item) => { item.policy.producers[0].scopes[0].component_ids = ["other"]; },
      /producer scope/u],
    [(item) => {
      const prior = item.policy.producers[0];
      item.policy.producers[0] = {
        producer_id: prior.producer_id,
        kind: "sigstore_keyless_github",
        key_epoch: prior.key_epoch,
        evidence_format: "provider_result_v1",
        scopes: prior.scopes,
        predicate_type:
          "https://uci-arena.example/attestations/provider-release-result/v1",
        oidc_issuer: "https://token.actions.githubusercontent.com",
        workflow_path: ".github/workflows/component-release.yml",
        workflow_name: "Component candidate",
        official_ref: "refs/heads/main",
        trigger: "workflow_dispatch",
        allow_diagnostic_refs: false,
        require_transparency_log: true,
        require_sct: true,
      };
    }, /not a local keyed producer/u],
    [(item) => { item.policy.producers[0].key_epoch = 0; }, /trust policy/u],
    [(item) => { item.policy.producers[0].public_key.sha256 = "0".repeat(64); },
      /public key digest/u],
  ];
  for (const [mutate, pattern] of cases) {
    const item = fixture();
    try {
      mutate(item);
      item.rewritePolicy();
      await assert.rejects(signLocalComponentProvenance(item.options(), {
        cosignRun() { assert.fail("Cosign must not run"); },
      }), pattern);
    } finally {
      item.cleanup();
    }
  }

  for (const mutate of [
    (item) => { item.evidence.component_id = "other"; },
    (item) => { item.evidence.source.repository = "example/other"; },
    (item) => { item.evidence.ownership = "installer_managed"; },
    (item) => { item.evidence.target_triple = "any-any"; },
  ]) {
    const item = fixture();
    try {
      mutate(item);
      item.rewriteEvidence();
      await assert.rejects(signLocalComponentProvenance(item.options(), {
        cosignRun() { assert.fail("Cosign must not run"); },
      }), /scope|canonical claim/u);
    } finally {
      item.cleanup();
    }
  }

  const noncanonical = fixture();
  try {
    noncanonical.evidence.dependencies = ["runtime.core", "alpha.core"];
    noncanonical.rewriteEvidence();
    await assert.rejects(signLocalComponentProvenance(noncanonical.options(), {
      cosignRun() { assert.fail("Cosign must not run"); },
    }), /canonical claim/u);
  } finally {
    noncanonical.cleanup();
  }
});

test("local signer rejects unsafe private-key references without disclosing them", async (t) => {
  const item = fixture();
  const repositoryKeyRoot = mkdtempSync(path.join(repositoryRoot, "build", "rp04-key-test-"));
  t.after(() => rmSync(repositoryKeyRoot, { recursive: true, force: true }));
  const repositoryKey = path.join(repositoryKeyRoot, "canary-repository.key");
  writeFileSync(repositoryKey, "key");
  const candidateKey = path.join(item.candidateRoot, "canary-candidate.key");
  writeFileSync(candidateKey, "key");
  const policyKey = path.join(item.policyRoot, "canary-policy.key");
  writeFileSync(policyKey, "key");
  const outputKey = path.join(item.outputRoot, "canary-output.key");
  writeFileSync(outputKey, "key");
  const linkPath = path.join(item.keyRoot, "canary-link.key");
  const candidates = [
    "relative-canary.key",
    repositoryKey,
    candidateKey,
    policyKey,
    outputKey,
    path.join(item.keyRoot, "missing-canary.key"),
  ];
  try {
    try {
      symlinkSync(item.signingKeyPath, linkPath, "file");
      candidates.push(linkPath);
    } catch (error) {
      if (error?.code !== "EPERM") throw error;
    }
    for (const signingKey of candidates) {
      let calls = 0;
      await assert.rejects(signLocalComponentProvenance(item.options({ signingKey }), {
        cosignRun() { calls += 1; },
      }), (error) => {
        assert.match(error.message, /signing key reference/u);
        assert.doesNotMatch(error.message, /canary|release\.key|engine\.tar/u);
        return true;
      });
      assert.equal(calls, 0);
    }
  } finally {
    item.cleanup();
  }
});

test("local signer never replaces an occupied output", async () => {
  const occupied = fixture();
  try {
    mkdirSync(occupied.outputDirectory);
    const marker = path.join(occupied.outputDirectory, "preserve.txt");
    writeFileSync(marker, "preserve");
    let calls = 0;
    await assert.rejects(signLocalComponentProvenance(occupied.options(), {
      cosignRun() { calls += 1; },
    }), /already exists/u);
    assert.equal(calls, 0);
    assert.equal(readFileSync(marker, "utf8"), "preserve");
  } finally {
    occupied.cleanup();
  }

  const repeated = fixture();
  try {
    const dependencies = {
      cosignRun: fakeCosign([]),
      verifyCandidate: verifiedFromRequest,
    };
    await signLocalComponentProvenance(repeated.options(), dependencies);
    const firstBundle = readFileSync(
      path.join(repeated.outputDirectory, "engine.tar.sigstore.json"),
    );
    await assert.rejects(
      signLocalComponentProvenance(repeated.options(), dependencies),
      /already exists/u,
    );
    assert.deepEqual(
      readFileSync(path.join(repeated.outputDirectory, "engine.tar.sigstore.json")),
      firstBundle,
    );
  } finally {
    repeated.cleanup();
  }
});

test("local signer cleans only its staging transaction after bounded failures", async () => {
  for (const failure of ["cosign", "partial", "verification"]) {
    const item = fixture();
    const collision = path.join(item.outputRoot, ".signed-candidate.staging-collision");
    mkdirSync(collision);
    writeFileSync(path.join(collision, "preserve.txt"), "preserve");
    try {
      const dependencies = failure === "verification"
        ? {
          cosignRun: fakeCosign([]),
          verifyCandidate() { throw new Error("immediate verification failed"); },
        }
        : {
          cosignRun: failure === "partial"
            ? fakeCosign([], { failAfterWrite: true })
            : () => { throw new Error("Cosign command failed (1)"); },
        };
      await assert.rejects(signLocalComponentProvenance(item.options(), dependencies));
      assert.equal(existsSync(item.outputDirectory), false);
      assert.equal(readFileSync(path.join(collision, "preserve.txt"), "utf8"), "preserve");
      assert.deepEqual(stagingEntries(item), [path.basename(collision)]);
    } finally {
      item.cleanup();
    }
  }
});

test("offline environment retains only execution state and optional password", () => {
  assert.deepEqual(buildOfflineEnvironment({
    PATH: "path",
    HOME: "home",
    COSIGN_PASSWORD: "password",
    COSIGN_PRIVATE_PASSWORD: "secret",
    GITHUB_TOKEN: "token",
    ACTIONS_ID_TOKEN_REQUEST_TOKEN: "oidc",
    SIGSTORE_ID_TOKEN: "sigstore",
    HTTPS_PROXY: "http://127.0.0.1:1",
  }), {
    PATH: "path",
    HOME: "home",
    COSIGN_PASSWORD: "password",
    HTTPS_PROXY: "http://127.0.0.1:1",
  });
});

test("pinned Cosign signs and immediately verifies offline with ephemeral keys", async (t) => {
  const cosignPath = path.join(
    repositoryRoot,
    "build",
    "tooling",
    "cosign-3.0.6",
    process.platform === "win32" ? "cosign.exe" : "cosign",
  );
  if (!existsSync(cosignPath)) {
    t.skip("pinned Cosign 3.0.6 is not present in the ignored tooling cache");
    return;
  }
  const keyRoot = temporaryRoot("rp04-real-key-");
  const secondKeyRoot = temporaryRoot("rp04-real-wrong-key-");
  const homeRoot = temporaryRoot("rp04-real-home-");
  t.after(() => {
    rmSync(keyRoot, { recursive: true, force: true });
    rmSync(secondKeyRoot, { recursive: true, force: true });
    rmSync(homeRoot, { recursive: true, force: true });
  });
  const password = `rp04-${randomBytes(16).toString("hex")}`;
  const offlineEnv = {
    ...process.env,
    HOME: homeRoot,
    USERPROFILE: homeRoot,
    XDG_CONFIG_HOME: path.join(homeRoot, "config"),
    XDG_CACHE_HOME: path.join(homeRoot, "cache"),
    COSIGN_PASSWORD: password,
    HTTP_PROXY: "http://127.0.0.1:1",
    HTTPS_PROXY: "http://127.0.0.1:1",
    ALL_PROXY: "http://127.0.0.1:1",
    NO_PROXY: "",
  };
  for (const root of [keyRoot, secondKeyRoot]) {
    runCosign({
      cosignPath,
      args: ["generate-key-pair", "--output-key-prefix", path.join(root, "release")],
      env: offlineEnv,
      timeoutMs: 180_000,
      maxOutputBytes: 1024 * 1024,
    });
  }
  const publicKeyBytes = readFileSync(path.join(keyRoot, "release.pub"));
  const publicKeyObject = createPublicKey(publicKeyBytes);
  const item = fixture({
    publicKeyBytes,
    publicKeyObject,
    signingKeyPath: path.join(keyRoot, "release.key"),
  });
  t.after(() => item.cleanup());
  const result = await signLocalComponentProvenance(item.options({
    cosign: cosignPath,
    env: offlineEnv,
  }));
  assert.equal(result.state, "verified");
  assert.deepEqual(
    readdirSync(item.outputDirectory).sort(),
    ["component-evidence.json", "engine.tar.sigstore.json"],
  );

  const wrongOutput = path.join(item.outputRoot, "wrong-key-candidate");
  await assert.rejects(signLocalComponentProvenance(item.options({
    signingKey: path.join(secondKeyRoot, "release.key"),
    outputDirectory: wrongOutput,
    cosign: cosignPath,
    env: offlineEnv,
  })), (error) => {
    assert.match(error.message, /Cosign command failed/u);
    assert.doesNotMatch(error.message, /release\.key|rp04-|COSIGN_PASSWORD/u);
    return true;
  });
  assert.equal(existsSync(wrongOutput), false);
});
