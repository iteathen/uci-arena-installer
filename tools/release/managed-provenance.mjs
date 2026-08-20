#!/usr/bin/env node

import { createHash } from "node:crypto";
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
import { runCosign } from "./cosign-cli.mjs";
import { parseDsseBundle, SIGSTORE_BUNDLE_MEDIA_TYPE } from "./sigstore-bundle.mjs";

export const MANAGED_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/managed-component-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export { SIGSTORE_BUNDLE_MEDIA_TYPE };

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
  return parseDsseBundle({ bytes: readFileSync(bundlePath), maximumBytes: 16 * 1024 * 1024 }).statement;
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
  runCosign({ cosignPath: executable, args: [
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
    "--new-bundle-format",
    request.artifactPath,
  ] });
}

function managedRef(result, producer, validationMode) {
  const prefix=`${INSTALLER_REPOSITORY}/${producer.workflow_path}@`;
  if(typeof result.workflow_identity!=="string"||!result.workflow_identity.startsWith(prefix)||!/#\d+\.\d+$/u.test(result.workflow_identity))throw new Error("managed result workflow_identity differs from the locked workflow");
  const ref=result.workflow_identity.slice(prefix.length).replace(/#\d+\.\d+$/u,"");
  if(validationMode==="official"&&ref!==producer.official_ref)throw new Error("managed result ref is not official");
  if(validationMode==="diagnostic"&&!producer.allow_diagnostic_refs&&ref!==producer.official_ref)throw new Error("producer does not allow diagnostic refs");
  if(!ref.startsWith("refs/heads/")||/[\u0000\r\n]/u.test(ref))throw new Error("managed result ref is invalid"); return ref;
}

function validateManagedResultShape(result) {
  const required=["schema","release_authority","suite_installer_authority","source_repository","source_commit","component_id","component_version","target_triple","artifact_name","artifact_sha256","artifact_size_bytes","installed_bytes","component_manifest_sha256","licenses_sha256","upstream_inputs","validation_evidence","workflow_identity"];
  const optional=["source_offer_url","file_count"];
  if(!result||typeof result!=="object"||Array.isArray(result)||required.some(key=>!(key in result))||Object.keys(result).some(key=>![...required,...optional].includes(key)))throw new Error("managed result fields are invalid");
  const sha=/^[0-9a-f]{64}$/u,commit=/^[0-9a-f]{40}$/u;
  if(!commit.test(result.source_commit??"")||!["artifact_sha256","component_manifest_sha256","licenses_sha256"].every(key=>sha.test(result[key]??""))||![result.artifact_size_bytes,result.installed_bytes].every(value=>Number.isSafeInteger(value)&&value>0)||!Array.isArray(result.upstream_inputs)||result.upstream_inputs.length<1||result.upstream_inputs.some(item=>!item||typeof item!=="object"||!new Set(["source","evaluation_network"]).has(item.role)||typeof item.url!=="string"||!item.url.startsWith("https://")||!sha.test(item.sha256??""))||!result.validation_evidence||typeof result.validation_evidence!=="object"||Array.isArray(result.validation_evidence))throw new Error("managed result structure is invalid");
}

export function verifyGitHubManagedCandidate({ assets, row, component, producer, cosignPath, validationMode="official", cosignVerify=executeManagedCosignVerification }) {
  void component;
  const result=assets.evidence.json;
  validateManagedResultShape(result);
  const expected={schema:"arena_managed_component_release_result_v1",component_id:row.component_id,component_version:row.component_version,artifact_name:row.artifact.name,artifact_sha256:row.artifact.sha256,component_manifest_sha256:row.component_manifest_sha256,source_repository:INSTALLER_REPOSITORY,source_commit:row.source_commit,release_authority:"installer_managed_component_only",suite_installer_authority:INSTALLER_REPOSITORY};
  for(const [field,value] of Object.entries(expected))if(result[field]!==value)throw new Error(`managed result ${field} differs from the locked candidate`);
  const target=result.target_triple==="any-any"?"any":result.target_triple;if(target!==row.target_triple||result.artifact_size_bytes!==assets.artifact.sizeBytes)throw new Error("managed result artifact identity differs from the locked candidate");
  if(row.artifact.type==="file_set"&&(result.file_count!==row.artifact.file_count||result.installed_bytes!==row.artifact.installed_bytes))throw new Error("managed result file-set metrics differ from the locked candidate");
  const ref=managedRef(result,producer,validationMode); const certificateIdentity=`https://github.com/${row.source_repository}/${producer.workflow_path}@${ref}`;
  cosignVerify(cosignPath,{artifactPath:assets.artifact.path,bundlePath:assets.bundle.path,certificateIdentity,oidcIssuer:producer.oidc_issuer,sourceCommit:row.source_commit,workflowName:producer.workflow_name,workflowRef:ref,workflowTrigger:producer.trigger});
  const parsed=parseDsseBundle({bytes:assets.bundle.bytes,maximumBytes:16*1024*1024});if(typeof parsed.verificationMaterial?.certificate?.rawBytes!=="string"||!Array.isArray(parsed.verificationMaterial?.tlogEntries)||parsed.verificationMaterial.tlogEntries.length<1||parsed.verificationMaterial.tlogEntries.some(entry=>!entry?.inclusionProof))throw new Error("Sigstore bundle lacks required keyless verification material");
  if(!isDeepStrictEqual(parsed.statement,buildExpectedManagedStatement(result)))throw new Error("Sigstore bundle payload does not equal the locked managed result");
  return Object.freeze({schema:"arena_verified_component_provenance_v1",component_id:row.component_id,component_version:row.component_version,target_triple:row.target_triple,ownership:"installer_managed",source_repository:row.source_repository,source_commit:row.source_commit,artifact:structuredClone(row.artifact),component_manifest_sha256:row.component_manifest_sha256,dependencies:[...row.dependencies].sort(),dependencies_verified_by:"release_lock",producer:{producer_id:producer.producer_id,kind:producer.kind,key_epoch:producer.key_epoch,audit_level:"transparency_logged"},evidence:structuredClone(row.evidence),bundle:{name:row.provenance.bundle_name,sha256:row.provenance.bundle_sha256}});
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
