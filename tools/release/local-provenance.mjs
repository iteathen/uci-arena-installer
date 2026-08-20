import { isDeepStrictEqual } from "node:util";
import { runCosign } from "./cosign-cli.mjs";
import { buildComponentProvenanceStatement, canonicalTargetTriple, validateComponentReleaseEvidence, validateVerifiedComponentProvenance } from "./component-provenance.mjs";
import { admitTrustedPublicKey, requireProducerScope } from "./provenance-trust-policy.mjs";
import { parseDsseBundle } from "./sigstore-bundle.mjs";

const PREDICATE="https://uci-arena.example/attestations/component-release-evidence/v1";

function offlineEnvironment(env) {
  const result={...env};
  for(const key of Object.keys(result))if(key.startsWith("GITHUB_")||key.startsWith("ACTIONS_")||key.startsWith("SIGSTORE_")||key.startsWith("COSIGN_"))delete result[key];
  return result;
}

export function executeLocalCosignVerification(cosignPath,{artifactPath,bundlePath,keyPath,env},cosignRun=runCosign) {
  return cosignRun({cosignPath,args:["verify-blob-attestation","--key",keyPath,"--bundle",bundlePath,"--type",PREDICATE,"--insecure-ignore-tlog","--new-bundle-format",artifactPath],env:offlineEnvironment(env)});
}

function requireLocalVerificationMaterial(value) {
  if(!value||typeof value!=="object"||Array.isArray(value)||Object.keys(value).length!==1||!("publicKey" in value))throw new Error("local Sigstore bundle verification material is invalid");
  const publicKey=value.publicKey;
  if(!publicKey||typeof publicKey!=="object"||Array.isArray(publicKey)||Object.keys(publicKey).some(key=>key!=="hint")||typeof publicKey.hint!=="string"||publicKey.hint.length>1024||/[\u0000\r\n]/u.test(publicKey.hint))throw new Error("local Sigstore bundle public-key material is invalid");
}

function requireEvidenceIdentity(evidence,row,component,producer) {
  if(producer.kind!=="sigstore_keyed_local"||producer.evidence_format!=="component_evidence_v1"||producer.predicate_type!==PREDICATE||producer.transparency_log!=="none"||!Number.isSafeInteger(producer.key_epoch)||producer.key_epoch<1)throw new Error("local producer contract is invalid");
  requireProducerScope(producer,{componentId:row.component_id,sourceRepository:row.source_repository,ownership:row.ownership});
  const expected={component_id:row.component_id,component_version:row.component_version,target_triple:canonicalTargetTriple(row.target_triple),ownership:row.ownership,source_repository:row.source_repository,source_commit:row.source_commit,artifact:row.artifact,component_manifest_sha256:row.component_manifest_sha256,dependencies:[...row.dependencies].sort()};
  const actual={component_id:evidence.component_id,component_version:evidence.component_version,target_triple:evidence.target_triple,ownership:evidence.ownership,source_repository:evidence.source.repository,source_commit:evidence.source.commit,artifact:evidence.artifact,component_manifest_sha256:evidence.component_manifest_sha256,dependencies:evidence.dependencies};
  if(!isDeepStrictEqual(actual,expected))throw new Error("local signed evidence differs from the locked candidate");
  if(component.component_id!==evidence.component_id||component.version!==evidence.component_version||component.repository!==evidence.source.repository||!isDeepStrictEqual([...component.requires].sort(),evidence.dependencies))throw new Error("local signed evidence differs from composition");
}

export function verifyLocalKeyedCandidate({assets,row,component,producer,trustPolicyPath,cosignPath,validationMode="official",env=process.env,cosignVerify=executeLocalCosignVerification}) {
  if(!new Set(["official","diagnostic"]).has(validationMode))throw new Error("local validation mode is invalid");
  const evidence=validateComponentReleaseEvidence(assets.evidence.json);
  requireEvidenceIdentity(evidence,row,component,producer);
  const key=admitTrustedPublicKey({trustPolicyPath,producer});
  cosignVerify(cosignPath,{artifactPath:assets.artifact.path,bundlePath:assets.bundle.path,keyPath:key.path,env});
  const parsed=parseDsseBundle({bytes:assets.bundle.bytes,maximumBytes:16*1024*1024});
  requireLocalVerificationMaterial(parsed.verificationMaterial);
  if(!isDeepStrictEqual(parsed.statement,buildComponentProvenanceStatement(evidence)))throw new Error("local Sigstore bundle payload does not equal the signed evidence");
  return validateVerifiedComponentProvenance({schema:"arena_verified_component_provenance_v1",component_id:evidence.component_id,component_version:evidence.component_version,target_triple:evidence.target_triple,ownership:evidence.ownership,source_repository:evidence.source.repository,source_commit:evidence.source.commit,artifact:structuredClone(evidence.artifact),component_manifest_sha256:evidence.component_manifest_sha256,dependencies:[...evidence.dependencies],dependencies_verified_by:"signed_evidence",producer:{producer_id:producer.producer_id,kind:producer.kind,key_epoch:producer.key_epoch,audit_level:"self_managed_key"},evidence:structuredClone(row.evidence),bundle:{name:row.provenance.bundle_name,sha256:row.provenance.bundle_sha256}});
}
