import { createHash } from "node:crypto";
import {
  lstatSync,
  readFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { isDeepStrictEqual } from "node:util";
import { resolveCosignExecutable as resolvePinnedCosign, runCosign } from "./cosign-cli.mjs";
import { parseDsseBundle, SIGSTORE_BUNDLE_MEDIA_TYPE } from "./sigstore-bundle.mjs";

export const PROVIDER_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/provider-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export { SIGSTORE_BUNDLE_MEDIA_TYPE };

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
  return parseDsseBundle({ bytes: readFileSync(bundlePath), maximumBytes: 16 * 1024 * 1024 }).statement;
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
  if (explicit) return resolvePinnedCosign({ override: explicit, platform, env: environment });
  const local = path.join(path.resolve(repositoryRoot), "build", "tooling", "cosign-3.0.6", platform === "win32" ? "cosign.exe" : "cosign");
  try { return resolvePinnedCosign({ override: local, platform, env: environment }); } catch { return resolvePinnedCosign({ platform, env: {} }); }
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
    "--new-bundle-format",
    request.artifactPath,
  ];
  runCosign({ cosignPath: executable, args });
}

function githubRef(result, producer, validationMode) {
  const prefix = `${result.source_repository}/${producer.workflow_path}@`;
  if (typeof result.workflow_identity !== "string" || !result.workflow_identity.startsWith(prefix) || !WORKFLOW_IDENTITY_SUFFIX.test(result.workflow_identity)) throw new Error("provider result workflow_identity differs from the locked workflow");
  const ref = result.workflow_identity.slice(prefix.length).replace(/#\d+\.\d+$/u, "");
  if (validationMode === "official" && ref !== producer.official_ref) throw new Error("provider result ref is not official");
  if (validationMode === "diagnostic" && !producer.allow_diagnostic_refs && ref !== producer.official_ref) throw new Error("producer does not allow diagnostic refs");
  if (!ref.startsWith("refs/heads/") || /[\u0000\r\n]/u.test(ref)) throw new Error("provider result ref is invalid");
  return ref;
}

function validateProviderResultShape(result) {
  const required=["schema","component_id","component_version","target_triple","artifact_name","artifact_sha256","artifact_size_bytes","component_manifest_sha256","source_repository","source_commit","source_tree_sha256","clean_tree","release_authority","suite_installer_authority","workflow_identity","toolchain_identity","dependency_lock_identities","build_flags","sbom_sha256","licenses_sha256","test_receipt_sha256","tests"];
  const optional=["artifact_type","artifact_kind","file_set_file_count","file_set_total_size_bytes","external_file_count","external_size_bytes","payload_included","validation_evidence"];
  if(!result||typeof result!=="object"||Array.isArray(result)||required.some(key=>!(key in result))||Object.keys(result).some(key=>![...required,...optional].includes(key)))throw new Error("provider result fields are invalid");
  const sha=/^[0-9a-f]{64}$/u, commit=/^[0-9a-f]{40}$/u;
  for(const key of ["artifact_sha256","component_manifest_sha256","source_tree_sha256","sbom_sha256","licenses_sha256","test_receipt_sha256"])if(!sha.test(result[key]??""))throw new Error(`provider result ${key} is invalid`);
  if(!commit.test(result.source_commit??"")||!Number.isSafeInteger(result.artifact_size_bytes)||result.artifact_size_bytes<1||!Array.isArray(result.build_flags)||result.build_flags.some(value=>typeof value!=="string")||!Array.isArray(result.tests)||result.tests.length<1||result.tests.some(value=>!value||typeof value!=="object"||Array.isArray(value))||!result.toolchain_identity||typeof result.toolchain_identity!=="object"||Array.isArray(result.toolchain_identity)||!result.dependency_lock_identities||typeof result.dependency_lock_identities!=="object"||Array.isArray(result.dependency_lock_identities))throw new Error("provider result structure is invalid");
}

export function verifyGitHubProviderCandidate({ assets, row, component, producer, cosignPath, validationMode = "official", cosignVerify = executeCosignVerification }) {
  const result = assets.evidence.json;
  validateProviderResultShape(result);
  const expected = { schema:"arena_provider_release_result_v1", component_id:row.component_id, component_version:row.component_version, artifact_name:row.artifact.name, artifact_sha256:row.artifact.sha256, source_repository:row.source_repository, source_commit:row.source_commit, clean_tree:true, release_authority:"provider_component_only", suite_installer_authority:INSTALLER_REPOSITORY, component_manifest_sha256:row.component_manifest_sha256 };
  for(const [field,value] of Object.entries(expected))if(result[field]!==value)throw new Error(`provider result ${field} differs from the locked candidate`);
  const target = result.target_triple === "any-any" ? "any" : result.target_triple;
  if(target!==row.target_triple||result.artifact_size_bytes!==assets.artifact.sizeBytes)throw new Error("provider result artifact identity differs from the locked candidate");
  const ref=githubRef(result,producer,validationMode);
  const certificateIdentity=`https://github.com/${row.source_repository}/${producer.workflow_path}@${ref}`;
  cosignVerify(cosignPath,{artifactPath:assets.artifact.path,bundlePath:assets.bundle.path,certificateIdentity,oidcIssuer:producer.oidc_issuer,repository:row.source_repository,sourceCommit:row.source_commit,workflowName:producer.workflow_name,workflowRef:ref,workflowTrigger:producer.trigger});
  const parsed=parseDsseBundle({bytes:assets.bundle.bytes,maximumBytes:16*1024*1024});
  if(typeof parsed.verificationMaterial?.certificate?.rawBytes!=="string"||!Array.isArray(parsed.verificationMaterial?.tlogEntries)||parsed.verificationMaterial.tlogEntries.length<1||parsed.verificationMaterial.tlogEntries.some(entry=>!entry?.inclusionProof))throw new Error("Sigstore bundle lacks required keyless verification material");
  if(!isDeepStrictEqual(parsed.statement,buildExpectedProviderStatement(result)))throw new Error("Sigstore bundle payload does not equal the locked provider result");
  return Object.freeze({schema:"arena_verified_component_provenance_v1",component_id:row.component_id,component_version:row.component_version,target_triple:row.target_triple,ownership:"provider",source_repository:row.source_repository,source_commit:row.source_commit,artifact:structuredClone(row.artifact),component_manifest_sha256:row.component_manifest_sha256,dependencies:[...row.dependencies].sort(),dependencies_verified_by:"release_lock",producer:{producer_id:producer.producer_id,kind:producer.kind,key_epoch:producer.key_epoch,audit_level:"transparency_logged"},evidence:structuredClone(row.evidence),bundle:{name:row.provenance.bundle_name,sha256:row.provenance.bundle_sha256}});
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
