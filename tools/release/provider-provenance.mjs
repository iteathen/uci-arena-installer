import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  lstatSync,
  readFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { isDeepStrictEqual } from "node:util";

export const PROVIDER_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/provider-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export const SIGSTORE_BUNDLE_MEDIA_TYPE =
  "application/vnd.dev.sigstore.bundle.v0.3+json";

const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";
const WORKFLOW_IDENTITY_SUFFIX = /#\d+\.\d+$/u;

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function exactFile(filename, expectedName, expectedSha256, label) {
  const resolved = path.resolve(filename);
  const status = lstatSync(resolved);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`${label} must be a nonempty regular file`);
  }
  if (path.basename(resolved) !== expectedName) {
    throw new Error(`${label} name differs from the release lock`);
  }
  if (sha256File(resolved) !== expectedSha256) {
    throw new Error(`${label} digest differs from the release lock`);
  }
  return { path: resolved, size: status.size };
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
  const bundle = readJson(bundlePath, "Sigstore bundle");
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
  return JSON.parse(decodeCanonicalBase64(bundle.dsseEnvelope.payload).toString("utf8"));
}

export function buildExpectedProviderStatement(providerResult) {
  return {
    _type: INTOTO_STATEMENT_TYPE,
    subject: [{
      name: providerResult.artifact_name,
      digest: { sha256: providerResult.artifact_sha256 },
    }],
    predicateType: PROVIDER_PREDICATE_TYPE,
    predicate: providerResult,
  };
}

export function resolveCosignExecutable({
  repositoryRoot,
  environment = process.env,
  platform = process.platform,
} = {}) {
  const explicit = environment.COSIGN_EXECUTABLE;
  if (explicit) {
    const candidate = path.resolve(explicit);
    const status = lstatSync(candidate);
    if (!status.isFile() || status.isSymbolicLink()) {
      throw new Error("COSIGN_EXECUTABLE must name a regular file");
    }
    return candidate;
  }
  const local = path.join(
    path.resolve(repositoryRoot),
    "build", "tooling", "cosign-3.0.6", platform === "win32" ? "cosign.exe" : "cosign",
  );
  try {
    const status = lstatSync(local);
    if (status.isFile() && !status.isSymbolicLink()) return local;
  } catch (error) {
    if (error?.code !== "ENOENT") throw error;
  }
  return "cosign";
}

export function executeCosignVerification(executable, request) {
  const args = [
    "verify-blob-attestation",
    "--bundle", request.bundlePath,
    "--certificate-identity", request.certificateIdentity,
    "--certificate-oidc-issuer", request.oidcIssuer,
    "--certificate-github-workflow-repository", request.repository,
    "--certificate-github-workflow-sha", request.sourceCommit,
    "--certificate-github-workflow-name", request.workflowName,
    "--certificate-github-workflow-ref", request.workflowRef,
    "--certificate-github-workflow-trigger", request.workflowTrigger,
    "--type", PROVIDER_PREDICATE_TYPE,
    request.artifactPath,
  ];
  const result = spawnSync(executable, args, {
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    const diagnostic = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim().length > 0)
      .join("\n");
    throw new Error(`Cosign provider verification failed: ${diagnostic || `exit ${result.status}`}`);
  }
}

export function verifyProviderCandidate({
  artifactPath,
  providerResultPath,
  bundlePath,
  lockRow,
  provider,
  cosignExecutable,
  cosignVerify = executeCosignVerification,
}) {
  const artifact = exactFile(
    artifactPath, lockRow.artifact_name, lockRow.artifact_sha256, "provider artifact",
  );
  const resultFile = exactFile(
    providerResultPath,
    lockRow.provider_result_name,
    lockRow.provider_result_sha256,
    "provider result",
  );
  const bundle = exactFile(
    bundlePath,
    lockRow.provenance_bundle_name,
    lockRow.provenance_bundle_sha256,
    "provider Sigstore bundle",
  );
  const result = readJson(resultFile.path, "provider result");
  const exactFields = {
    schema: "arena_provider_release_result_v1",
    component_id: lockRow.component_id,
    component_version: lockRow.component_version,
    target_triple: lockRow.target_triple,
    artifact_name: lockRow.artifact_name,
    artifact_sha256: lockRow.artifact_sha256,
    source_repository: provider.repository,
    source_commit: lockRow.source_commit,
    clean_tree: true,
    release_authority: "provider_component_only",
    suite_installer_authority: INSTALLER_REPOSITORY,
  };
  for (const [field, expected] of Object.entries(exactFields)) {
    if (result[field] !== expected) {
      throw new Error(`provider result ${field} differs from the locked candidate`);
    }
  }
  if (result.artifact_size_bytes !== artifact.size) {
    throw new Error("provider result artifact_size_bytes differs from the artifact");
  }
  const workflowPrefix =
    `${provider.repository}/${provider.workflow_path}@${lockRow.provenance_workflow_ref}`;
  if (typeof result.workflow_identity !== "string"
      || !result.workflow_identity.startsWith(`${workflowPrefix}#`)
      || !WORKFLOW_IDENTITY_SUFFIX.test(result.workflow_identity)) {
    throw new Error("provider result workflow_identity differs from the locked workflow");
  }
  cosignVerify(cosignExecutable, {
    artifactPath: artifact.path,
    bundlePath: bundle.path,
    certificateIdentity: lockRow.provenance_certificate_identity,
    oidcIssuer: lockRow.provenance_oidc_issuer,
    repository: provider.repository,
    sourceCommit: lockRow.source_commit,
    workflowName: lockRow.provenance_workflow_name,
    workflowRef: lockRow.provenance_workflow_ref,
    workflowTrigger: lockRow.provenance_workflow_trigger,
  });
  const actualStatement = readBundleStatement(bundle.path);
  const expectedStatement = buildExpectedProviderStatement(result);
  if (!isDeepStrictEqual(actualStatement, expectedStatement)) {
    throw new Error("Sigstore bundle payload does not equal the locked provider result");
  }
  return {
    schema: "arena_verified_provider_candidate_v1",
    component_id: result.component_id,
    component_version: result.component_version,
    target_triple: result.target_triple,
    artifact_path: artifact.path,
    provider_result: result,
  };
}
