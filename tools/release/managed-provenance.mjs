#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  appendFileSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";

export const MANAGED_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/managed-component-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export const SIGSTORE_BUNDLE_MEDIA_TYPE =
  "application/vnd.dev.sigstore.bundle.v0.3+json";

const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";
const COMMIT_PATTERN = /^[0-9a-f]{40}$/u;

function canonicalJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function readJson(filename) {
  return JSON.parse(readFileSync(filename, "utf8"));
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function decodeCanonicalBase64(value) {
  if (typeof value !== "string"
      || value.length === 0
      || value.length % 4 !== 0
      || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  const decoded = Buffer.from(value, "base64");
  if (decoded.toString("base64") !== value) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  return decoded;
}

function readBundleStatement(bundlePath) {
  const bundle = readJson(bundlePath);
  if (bundle?.mediaType !== SIGSTORE_BUNDLE_MEDIA_TYPE) {
    throw new Error("unsupported Sigstore bundle media type");
  }
  if (typeof bundle?.verificationMaterial?.certificate?.rawBytes !== "string"
      || bundle.verificationMaterial.certificate.rawBytes.length === 0) {
    throw new Error("Sigstore bundle is missing its signing certificate");
  }
  const logEntries = bundle.verificationMaterial.tlogEntries;
  if (!Array.isArray(logEntries)
      || logEntries.length === 0
      || logEntries.some((entry) => !entry?.inclusionProof)) {
    throw new Error("Sigstore bundle is missing a transparency-log inclusion proof");
  }
  if (bundle?.dsseEnvelope?.payloadType !== "application/vnd.in-toto+json") {
    throw new Error("Sigstore bundle does not contain an in-toto DSSE payload");
  }
  if (!Array.isArray(bundle.dsseEnvelope.signatures)
      || bundle.dsseEnvelope.signatures.length !== 1) {
    throw new Error("Sigstore bundle must contain exactly one DSSE signature");
  }
  return JSON.parse(
    decodeCanonicalBase64(bundle.dsseEnvelope.payload).toString("utf8"),
  );
}

export function buildExpectedManagedStatement(managedResult) {
  return {
    _type: INTOTO_STATEMENT_TYPE,
    subject: [{
      name: managedResult.artifact_name,
      digest: { sha256: managedResult.artifact_sha256 },
    }],
    predicateType: MANAGED_PREDICATE_TYPE,
    predicate: managedResult,
  };
}

export function prepareManagedProvenance({
  artifactPath,
  managedResultPath,
  statementDirectory,
}) {
  const artifact = path.resolve(artifactPath);
  const resultPath = path.resolve(managedResultPath);
  const managedResult = readJson(resultPath);
  if (managedResult.schema !== "arena_managed_component_release_result_v1"
      || managedResult.release_authority !== "installer_managed_component_only"
      || managedResult.suite_installer_authority !== INSTALLER_REPOSITORY
      || managedResult.source_repository !== INSTALLER_REPOSITORY
      || !COMMIT_PATTERN.test(managedResult.source_commit ?? "")
      || path.basename(artifact) !== managedResult.artifact_name
      || sha256File(artifact) !== managedResult.artifact_sha256) {
    throw new Error("managed result does not bind the exact Installer artifact");
  }
  const statement = buildExpectedManagedStatement(managedResult);
  const statementRoot = path.resolve(statementDirectory);
  mkdirSync(statementRoot, { recursive: true });
  const bundlePath = `${artifact}.sigstore.json`;
  const statementPath = path.join(
    statementRoot,
    `${managedResult.artifact_name}.intoto.json`,
  );
  writeFileSync(statementPath, canonicalJson(statement), { flag: "wx" });
  return {
    artifactPath: artifact,
    bundlePath,
    predicateType: MANAGED_PREDICATE_TYPE,
    statement,
    statementPath,
  };
}

export function verifyBundlePayload(bundlePath, statementPath) {
  const actual = readBundleStatement(bundlePath);
  const expected = readJson(statementPath);
  if (!isDeepStrictEqual(actual, expected)) {
    throw new Error("Sigstore bundle payload does not match the expected statement");
  }
  return actual;
}

function exactLockedFile(filename, expectedName, expectedSha256, label) {
  const resolved = path.resolve(filename);
  const status = lstatSync(resolved);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`${label} must be a nonempty regular file`);
  }
  if (path.basename(resolved) !== expectedName || sha256File(resolved) !== expectedSha256) {
    throw new Error(`${label} identity differs from the release lock`);
  }
  return { path: resolved, size: status.size };
}

function executeManagedCosignVerification(executable, request) {
  const result = spawnSync(executable, [
    "verify-blob-attestation",
    "--bundle", request.bundlePath,
    "--certificate-identity", request.certificateIdentity,
    "--certificate-oidc-issuer", request.oidcIssuer,
    "--certificate-github-workflow-repository", INSTALLER_REPOSITORY,
    "--certificate-github-workflow-sha", request.sourceCommit,
    "--certificate-github-workflow-name", request.workflowName,
    "--certificate-github-workflow-ref", request.workflowRef,
    "--certificate-github-workflow-trigger", request.workflowTrigger,
    "--type", MANAGED_PREDICATE_TYPE,
    request.artifactPath,
  ], { encoding: "utf8", windowsHide: true });
  if (result.error || result.status !== 0) {
    const diagnostic = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim().length > 0)
      .join("\n");
    throw new Error(`Cosign managed verification failed: ${diagnostic || `exit ${result.status}`}`);
  }
}

export function verifyManagedCandidate({
  artifactPath,
  managedResultPath,
  bundlePath,
  lockRow,
  cosignExecutable,
  cosignVerify = executeManagedCosignVerification,
}) {
  const artifact = exactLockedFile(
    artifactPath, lockRow.artifact_name, lockRow.artifact_sha256, "managed artifact",
  );
  const resultFile = exactLockedFile(
    managedResultPath,
    lockRow.managed_result_name,
    lockRow.managed_result_sha256,
    "managed result",
  );
  const bundle = exactLockedFile(
    bundlePath,
    lockRow.provenance_bundle_name,
    lockRow.provenance_bundle_sha256,
    "managed Sigstore bundle",
  );
  const managedResult = readJson(resultFile.path);
  const exactFields = {
    schema: "arena_managed_component_release_result_v1",
    component_id: lockRow.component_id,
    component_version: lockRow.component_version,
    target_triple: lockRow.target_triple,
    artifact_name: lockRow.artifact_name,
    artifact_sha256: lockRow.artifact_sha256,
    component_manifest_sha256: lockRow.component_manifest_sha256,
    licenses_sha256: lockRow.licenses_sha256,
    source_repository: INSTALLER_REPOSITORY,
    source_commit: lockRow.source_commit,
    release_authority: "installer_managed_component_only",
    suite_installer_authority: INSTALLER_REPOSITORY,
  };
  for (const [field, expected] of Object.entries(exactFields)) {
    if (managedResult[field] !== expected) {
      throw new Error(`managed result ${field} differs from the locked candidate`);
    }
  }
  if (managedResult.artifact_size_bytes !== artifact.size) {
    throw new Error("managed result artifact_size_bytes differs from the artifact");
  }
  const workflowPrefix =
    `${INSTALLER_REPOSITORY}/.github/workflows/managed-component-release.yml`
    + `@${lockRow.provenance_workflow_ref}#`;
  if (typeof managedResult.workflow_identity !== "string"
      || !managedResult.workflow_identity.startsWith(workflowPrefix)
      || !/#\d+\.\d+$/u.test(managedResult.workflow_identity)) {
    throw new Error("managed result workflow_identity differs from the locked workflow");
  }
  cosignVerify(cosignExecutable, {
    artifactPath: artifact.path,
    bundlePath: bundle.path,
    certificateIdentity: lockRow.provenance_certificate_identity,
    oidcIssuer: lockRow.provenance_oidc_issuer,
    sourceCommit: lockRow.source_commit,
    workflowName: lockRow.provenance_workflow_name,
    workflowRef: lockRow.provenance_workflow_ref,
    workflowTrigger: lockRow.provenance_workflow_trigger,
  });
  const actual = readBundleStatement(bundle.path);
  const expected = buildExpectedManagedStatement(managedResult);
  if (!isDeepStrictEqual(actual, expected)) {
    throw new Error("Sigstore bundle payload does not equal the locked managed result");
  }
  return {
    schema: "arena_verified_managed_candidate_v1",
    component_id: managedResult.component_id,
    component_version: managedResult.component_version,
    target_triple: managedResult.target_triple,
    artifact_path: artifact.path,
    managed_result: managedResult,
  };
}

function parseArguments(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    values[flag.slice(2)] = value;
  }
  return values;
}

function writeGitHubOutputs(filename, values) {
  for (const [name, value] of Object.entries(values)) {
    const output = String(value);
    if (/[\u0000-\u001f\u007f]/u.test(output)) {
      throw new Error(`unsafe GitHub output value for ${name}`);
    }
    appendFileSync(filename, `${name}=${output}\n`, "utf8");
  }
}

function main(argv) {
  const [command, ...rest] = argv;
  const args = parseArguments(rest);
  if (command === "prepare") {
    const prepared = prepareManagedProvenance({
      artifactPath: args.artifact,
      managedResultPath: args.result,
      statementDirectory: args["statement-directory"],
    });
    if (args["github-output"]) {
      writeGitHubOutputs(args["github-output"], {
        artifact_path: prepared.artifactPath,
        bundle_path: prepared.bundlePath,
        predicate_type: prepared.predicateType,
        statement_path: prepared.statementPath,
      });
    } else {
      process.stdout.write(canonicalJson(prepared));
    }
    return;
  }
  if (command === "verify-bundle") {
    verifyBundlePayload(args.bundle, args.statement);
    process.stdout.write("managed-provenance: bundle payload verified\n");
    return;
  }
  throw new Error("usage: managed-provenance.mjs <prepare|verify-bundle> [options]");
}

if (process.argv[1]
    && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`managed-provenance: ${error.message}\n`);
    process.exitCode = 1;
  }
}
