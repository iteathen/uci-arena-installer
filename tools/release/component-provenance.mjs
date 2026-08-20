const ID = /^[A-Za-z0-9._-]{1,128}$/;
const VERSION = /^[A-Za-z0-9._-]{1,64}$/;
const REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;
const SHA256 = /^[0-9a-f]{64}$/;
const COMMIT = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/;
const ASSET = /^[A-Za-z0-9._-]{1,255}$/;
const MATERIAL_URI = /^(?:https:\/\/|git\+https:\/\/|urn:|file-name:)[^\u0000\r\n]+$/;
const TARGETS = new Set(["windows-x86_64", "linux-x86_64", "any"]);

function fail(message) { throw new Error(`invalid component provenance: ${message}`); }
function object(value, name) { if (!value || typeof value !== "object" || Array.isArray(value)) fail(name); }
function exact(value, keys, name) {
  object(value, name);
  const actual = Object.keys(value).sort();
  const wanted = [...keys].sort();
  if (actual.length !== wanted.length || actual.some((key, i) => key !== wanted[i])) fail(`${name} fields`);
}
function text(value, pattern, name, max = Infinity) {
  if (typeof value !== "string" || value.length > max || !pattern.test(value)) fail(name);
}
function integer(value, name) { if (!Number.isSafeInteger(value) || value < 1) fail(name); }
function digest(value, name) { text(value, SHA256, name); }
function uniqueStrings(values, name, max) {
  if (!Array.isArray(values) || values.length > max) fail(name);
  const seen = new Set();
  for (const value of values) { text(value, ID, name); if (seen.has(value)) fail(`${name} duplicate`); seen.add(value); }
  return [...values].sort();
}
function clone(value) { return structuredClone(value); }
function deepFreeze(value) {
  if (value && typeof value === "object" && !Object.isFrozen(value)) {
    for (const child of Object.values(value)) deepFreeze(child);
    Object.freeze(value);
  }
  return value;
}
function validateArtifact(value) {
  object(value, "artifact");
  const required = ["name", "sha256", "size_bytes", "type"];
  const optional = value.type === "file_set" ? ["file_count", "installed_bytes"] : [];
  exact(value, [...required, ...optional], "artifact");
  text(value.name, ASSET, "artifact name"); digest(value.sha256, "artifact digest"); integer(value.size_bytes, "artifact size");
  if (!new Set(["archive", "file_set"]).has(value.type)) fail("artifact type");
  if (value.type === "file_set") { integer(value.file_count, "file count"); integer(value.installed_bytes, "installed bytes"); }
  return clone(value);
}

export function canonicalTargetTriple(value) {
  if (value === "any-any") return "any";
  if (!TARGETS.has(value)) fail("target triple");
  return value;
}

export function validateComponentReleaseEvidence(candidate) {
  exact(candidate, ["schema", "schema_version", "component_id", "component_version", "target_triple", "ownership", "source", "artifact", "component_manifest_sha256", "dependencies", "build"], "evidence");
  if (candidate.schema !== "arena_component_release_evidence_v1" || candidate.schema_version !== 1) fail("evidence schema");
  text(candidate.component_id, ID, "component id"); text(candidate.component_version, VERSION, "component version");
  const target_triple = canonicalTargetTriple(candidate.target_triple);
  if (!new Set(["provider", "installer_managed"]).has(candidate.ownership)) fail("ownership");
  exact(candidate.source, ["repository", "commit", "tree_sha256", "clean_tree"], "source");
  text(candidate.source.repository, REPOSITORY, "repository", 256); text(candidate.source.commit, COMMIT, "commit"); digest(candidate.source.tree_sha256, "tree digest");
  if (candidate.source.clean_tree !== true) fail("clean tree");
  const artifact = validateArtifact(candidate.artifact); digest(candidate.component_manifest_sha256, "manifest digest");
  const dependencies = uniqueStrings(candidate.dependencies, "dependencies", 256);
  exact(candidate.build, ["builder_id", "invocation_id", "toolchain_identity_sha256", "tests_passed", "materials", "evidence_assets"], "build");
  text(candidate.build.builder_id, /^[A-Za-z0-9][A-Za-z0-9._:/@-]*$/, "builder id", 256);
  text(candidate.build.invocation_id, /^[A-Za-z0-9][A-Za-z0-9._:/#@-]*$/, "invocation id", 256);
  digest(candidate.build.toolchain_identity_sha256, "toolchain digest"); if (candidate.build.tests_passed !== true) fail("tests");
  if (!Array.isArray(candidate.build.materials) || candidate.build.materials.length > 1024) fail("materials");
  const materialKeys = new Set();
  for (const item of candidate.build.materials) {
    exact(item, ["role", "uri", "sha256"], "material");
    if (!new Set(["source", "dependency", "runtime", "model", "network", "toolchain", "other"]).has(item.role)) fail("material role");
    text(item.uri, MATERIAL_URI, "material uri", 2048); digest(item.sha256, "material digest");
    const key = `${item.role}\0${item.uri}`; if (materialKeys.has(key)) fail("material duplicate"); materialKeys.add(key);
  }
  if (!Array.isArray(candidate.build.evidence_assets) || candidate.build.evidence_assets.length < 2 || candidate.build.evidence_assets.length > 64) fail("evidence assets");
  const names = new Set(), pairs = new Set(), kinds = new Set();
  for (const item of candidate.build.evidence_assets) {
    exact(item, ["kind", "name", "sha256", "size_bytes"], "evidence asset");
    if (!new Set(["sbom", "licenses", "test_receipt", "build_details", "source_offer", "other"]).has(item.kind)) fail("evidence kind");
    text(item.name, ASSET, "evidence name"); digest(item.sha256, "evidence digest"); integer(item.size_bytes, "evidence size");
    const pair = `${item.kind}\0${item.name}`; if (names.has(item.name) || pairs.has(pair)) fail("evidence asset duplicate");
    names.add(item.name); pairs.add(pair); kinds.add(item.kind);
  }
  if (!kinds.has("licenses") || !kinds.has("test_receipt")) fail("required evidence assets");
  const result = clone(candidate); result.target_triple = target_triple; result.dependencies = dependencies;
  return deepFreeze(result);
}

export function buildComponentProvenanceStatement(evidence) {
  const predicate = validateComponentReleaseEvidence(evidence);
  return deepFreeze({ _type: "https://in-toto.io/Statement/v1", subject: [{ name: predicate.artifact.name, digest: { sha256: predicate.artifact.sha256 } }], predicateType: "https://uci-arena.example/attestations/component-release-evidence/v1", predicate: clone(predicate) });
}

export function validateComponentProvenanceStatement(statement) {
  exact(statement, ["_type", "subject", "predicateType", "predicate"], "statement");
  if (statement._type !== "https://in-toto.io/Statement/v1" || statement.predicateType !== "https://uci-arena.example/attestations/component-release-evidence/v1") fail("statement type");
  if (!Array.isArray(statement.subject) || statement.subject.length !== 1) fail("statement subject");
  exact(statement.subject[0], ["name", "digest"], "subject"); exact(statement.subject[0].digest, ["sha256"], "subject digest");
  const predicate = validateComponentReleaseEvidence(statement.predicate);
  if (statement.subject[0].name !== predicate.artifact.name || statement.subject[0].digest.sha256 !== predicate.artifact.sha256) fail("subject mismatch");
  return deepFreeze(clone(statement));
}

export function validateVerifiedComponentProvenance(candidate) {
  exact(candidate, ["schema", "component_id", "component_version", "target_triple", "ownership", "source_repository", "source_commit", "artifact", "component_manifest_sha256", "dependencies", "dependencies_verified_by", "producer", "evidence", "bundle"], "verified result");
  if (candidate.schema !== "arena_verified_component_provenance_v1") fail("verified schema");
  text(candidate.component_id, ID, "component id"); text(candidate.component_version, VERSION, "component version"); canonicalTargetTriple(candidate.target_triple);
  if (!new Set(["provider", "installer_managed"]).has(candidate.ownership)) fail("ownership");
  text(candidate.source_repository, REPOSITORY, "repository", 256); text(candidate.source_commit, COMMIT, "commit"); validateArtifact(candidate.artifact); digest(candidate.component_manifest_sha256, "manifest digest");
  uniqueStrings(candidate.dependencies, "dependencies", 256);
  exact(candidate.producer, ["producer_id", "kind", "key_epoch", "audit_level"], "producer"); text(candidate.producer.producer_id, ID, "producer id"); integer(candidate.producer.key_epoch, "key epoch");
  exact(candidate.evidence, ["format", "name", "sha256"], "verified evidence"); text(candidate.evidence.name, ASSET, "evidence name"); digest(candidate.evidence.sha256, "evidence digest");
  exact(candidate.bundle, ["name", "sha256"], "bundle"); text(candidate.bundle.name, ASSET, "bundle name"); digest(candidate.bundle.sha256, "bundle digest");
  const keyed = candidate.producer.kind === "sigstore_keyed_local";
  const keyless = candidate.producer.kind === "sigstore_keyless_github";
  if ((!keyed && !keyless) || (keyed && (candidate.producer.audit_level !== "self_managed_key" || candidate.dependencies_verified_by !== "signed_evidence" || candidate.evidence.format !== "component_evidence_v1")) || (keyless && (candidate.producer.audit_level !== "transparency_logged" || candidate.dependencies_verified_by !== "release_lock" || !new Set(["provider_result_v1", "managed_result_v1"]).has(candidate.evidence.format)))) fail("producer combination");
  return deepFreeze(clone(candidate));
}

export function compareVerifiedProvenance({ verified, row, component, producer }) {
  const value = validateVerifiedComponentProvenance(verified);
  const expected = {
    component_id: row.component_id, component_version: row.component_version, target_triple: canonicalTargetTriple(row.target_triple), ownership: row.ownership,
    source_repository: row.source_repository, source_commit: row.source_commit, artifact: row.artifact, component_manifest_sha256: row.component_manifest_sha256,
    dependencies: [...row.dependencies].sort(), evidence: row.evidence, bundle: { name: row.provenance.bundle_name, sha256: row.provenance.bundle_sha256 }
  };
  for (const [key, item] of Object.entries(expected)) if (JSON.stringify(value[key]) !== JSON.stringify(item)) fail(`verified mismatch: ${key}`);
  if (component.component_id !== value.component_id || component.version !== value.component_version || component.repository !== value.source_repository || JSON.stringify([...component.requires].sort()) !== JSON.stringify(value.dependencies)) fail("composition mismatch");
  if (producer.producer_id !== value.producer.producer_id || producer.kind !== value.producer.kind || producer.key_epoch !== value.producer.key_epoch || producer.evidence_format !== value.evidence.format || row.provenance.producer_id !== value.producer.producer_id) fail("producer mismatch");
  return value;
}

export async function verifyComponentCandidate({ row, component, candidateDirectory, trustPolicy, trustPolicyDirectory, cosignPath, validationMode="official" }) {
  const policy=validateProvenanceTrustPolicy(trustPolicy);
  const producer=producerById(policy,row?.provenance?.producer_id);
  requireProducerScope(producer,{componentId:row.component_id,sourceRepository:row.source_repository,ownership:row.ownership});
  const assets=await admitCandidateAssets({row,candidateDirectory});
  let verified;
  if(producer.kind==="sigstore_keyed_local"&&producer.evidence_format==="component_evidence_v1"){
    const {verifyLocalKeyedCandidate}=await import("./local-provenance.mjs");
    verified=verifyLocalKeyedCandidate({assets,row,component,producer,trustPolicyPath:trustPolicyDirectory,cosignPath,validationMode});
  }
  else if(producer.kind==="sigstore_keyless_github"&&producer.evidence_format==="provider_result_v1")verified=verifyGitHubProviderCandidate({assets,row,component,producer,cosignPath,validationMode});
  else if(producer.kind==="sigstore_keyless_github"&&producer.evidence_format==="managed_result_v1")verified=verifyGitHubManagedCandidate({assets,row,component,producer,cosignPath,validationMode});
  else throw new Error("unsupported_producer_adapter");
  return compareVerifiedProvenance({verified:validateVerifiedComponentProvenance(verified),row,component,producer});
}
import { admitCandidateAssets } from "./candidate-assets.mjs";
import { verifyGitHubManagedCandidate } from "./managed-provenance.mjs";
import { producerById, requireProducerScope, validateProvenanceTrustPolicy } from "./provenance-trust-policy.mjs";
import { verifyGitHubProviderCandidate } from "./provider-provenance.mjs";
