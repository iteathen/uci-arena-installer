import path from "node:path";
import { createHash, createPublicKey } from "node:crypto";
import { lstatSync, readFileSync, realpathSync } from "node:fs";

const ID = /^[A-Za-z0-9._-]{1,128}$/;
const REPO = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/;
const SHA = /^[0-9a-f]{64}$/;
const RELATIVE = /^(?![A-Za-z]:)(?![/\\])(?!.*(?:^|[/\\])\.\.(?:[/\\]|$))[A-Za-z0-9._/-]+$/;
function fail(message) { throw new Error(`invalid provenance trust policy: ${message}`); }
function exact(value, keys, name) { if (!value || typeof value !== "object" || Array.isArray(value)) fail(name); const a=Object.keys(value).sort(), b=[...keys].sort(); if(a.length!==b.length||a.some((k,i)=>k!==b[i])) fail(`${name} fields`); }
function integer(value, name) { if (!Number.isSafeInteger(value) || value < 1) fail(name); }
function match(value, regex, name, max=Infinity) { if(typeof value!=="string"||value.length>max||!regex.test(value)) fail(name); }
function frozen(value) { if(value&&typeof value==="object"){for(const child of Object.values(value)) frozen(child); Object.freeze(value);} return value; }

export function validateProvenanceTrustPolicy(candidate) {
  exact(candidate,["schema","schema_version","policy_id","policy_version","suite_installer_authority","cosign_version","producers"],"policy");
  if(candidate.schema!=="arena_provenance_trust_policy_v1"||candidate.schema_version!==1||candidate.suite_installer_authority!=="iteathen/uci-arena-installer"||candidate.cosign_version!=="3.0.6") fail("policy constants");
  match(candidate.policy_id,ID,"policy id"); integer(candidate.policy_version,"policy version");
  if(!Array.isArray(candidate.producers)||candidate.producers.length<1||candidate.producers.length>64) fail("producers");
  const ids=new Set();
  for(const producer of candidate.producers){
    const github=producer?.kind==="sigstore_keyless_github";
    const fields=github?["producer_id","kind","key_epoch","evidence_format","scopes","predicate_type","oidc_issuer","workflow_path","workflow_name","official_ref","trigger","allow_diagnostic_refs","require_transparency_log","require_sct"]:["producer_id","kind","key_epoch","evidence_format","scopes","predicate_type","public_key","transparency_log"];
    exact(producer,fields,"producer"); match(producer.producer_id,ID,"producer id"); if(ids.has(producer.producer_id)) fail("duplicate producer"); ids.add(producer.producer_id); integer(producer.key_epoch,"key epoch");
    if(!Array.isArray(producer.scopes)||producer.scopes.length<1||producer.scopes.length>64) fail("scopes"); const scopes=new Set();
    for(const scope of producer.scopes){exact(scope,["source_repository","ownership","component_ids"],"scope"); match(scope.source_repository,REPO,"scope repository",256); if(!new Set(["provider","installer_managed"]).has(scope.ownership)||!Array.isArray(scope.component_ids)||scope.component_ids.length<1||scope.component_ids.length>256) fail("scope"); const components=new Set(); for(const id of scope.component_ids){match(id,ID,"component id"); if(components.has(id))fail("duplicate component scope"); components.add(id);} const key=`${scope.source_repository}\0${scope.ownership}`; if(scopes.has(key))fail("duplicate scope"); scopes.add(key);}
    if(github){
      const predicates={provider_result_v1:"https://uci-arena.example/attestations/provider-release-result/v1",managed_result_v1:"https://uci-arena.example/attestations/managed-component-release-result/v1"};
      if(producer.predicate_type!==predicates[producer.evidence_format]||producer.oidc_issuer!=="https://token.actions.githubusercontent.com"||!/^[.]github\/workflows\/[A-Za-z0-9._-]+[.]ya?ml$/.test(producer.workflow_path)||typeof producer.workflow_name!=="string"||producer.workflow_name.length<1||producer.workflow_name.length>128||/[\u0000\r\n]/.test(producer.workflow_name)||producer.official_ref!=="refs/heads/main"||producer.trigger!=="workflow_dispatch"||typeof producer.allow_diagnostic_refs!=="boolean"||producer.require_transparency_log!==true||producer.require_sct!==true) fail("GitHub producer");
    } else {
      if(producer.kind!=="sigstore_keyed_local"||producer.evidence_format!=="component_evidence_v1"||producer.predicate_type!=="https://uci-arena.example/attestations/component-release-evidence/v1"||producer.transparency_log!=="none") fail("local producer");
      exact(producer.public_key,["path","sha256","spki_sha256"],"public key"); match(producer.public_key.path,RELATIVE,"public key path",512); match(producer.public_key.sha256,SHA,"public key digest"); match(producer.public_key.spki_sha256,SHA,"SPKI digest");
    }
  }
  return frozen(structuredClone(candidate));
}

export function producerById(policy, producerId) {
  const validated=validateProvenanceTrustPolicy(policy); match(producerId,ID,"producer id"); const producer=validated.producers.find(item=>item.producer_id===producerId); if(!producer) fail("unknown producer"); return producer;
}

export function requireProducerScope(producer,{componentId,sourceRepository,ownership}) {
  match(componentId,ID,"component id"); match(sourceRepository,REPO,"repository",256);
  const scope=producer.scopes.find(item=>item.source_repository===sourceRepository&&item.ownership===ownership&&item.component_ids.includes(componentId)); if(!scope) fail("producer scope"); return scope;
}

export function resolveTrustedPublicKeyPath(policyPath,producer) {
  if(producer.kind!=="sigstore_keyed_local") fail("producer has no public key");
  const base=path.dirname(path.resolve(policyPath)); const resolved=path.resolve(base,producer.public_key.path); const relative=path.relative(base,resolved);
  if(relative===""||relative.startsWith("..")||path.isAbsolute(relative)) fail("public key containment"); return resolved;
}

function sameIdentity(before, after) {
  return before.dev===after.dev&&before.ino===after.ino&&before.size===after.size&&before.mtimeNs===after.mtimeNs;
}

export function admitTrustedPublicKey({ trustPolicyPath, producer }) {
  if(typeof trustPolicyPath!=="string"||!path.isAbsolute(trustPolicyPath)||path.normalize(trustPolicyPath)!==trustPolicyPath)fail("trust policy path");
  if(producer?.kind!=="sigstore_keyed_local")fail("producer has no public key");
  match(producer.public_key?.path,RELATIVE,"public key path",512);match(producer.public_key?.sha256,SHA,"public key digest");match(producer.public_key?.spki_sha256,SHA,"SPKI digest");
  let policyStatus;
  try{policyStatus=lstatSync(trustPolicyPath,{bigint:true});}catch{fail("trust policy path unavailable");}
  if(!policyStatus.isFile()||policyStatus.isSymbolicLink())fail("trust policy path");
  const base=path.dirname(trustPolicyPath),resolved=resolveTrustedPublicKeyPath(trustPolicyPath,producer);
  try{
    if(realpathSync.native(base)!==base||realpathSync.native(resolved)!==resolved)fail("public key realpath");
  }catch(error){if(error?.message?.startsWith("invalid provenance trust policy:"))throw error;fail("public key realpath");}
  let before;try{before=lstatSync(resolved,{bigint:true});}catch{fail("public key unavailable");}
  if(!before.isFile()||before.isSymbolicLink()||before.size<1n||before.size>1024n*1024n)fail("public key file");
  let bytes;try{bytes=readFileSync(resolved);}catch{fail("public key read");}
  let after;try{after=lstatSync(resolved,{bigint:true});}catch{fail("public key changed");}
  if(!sameIdentity(before,after)||BigInt(bytes.byteLength)!==before.size)fail("public key changed");
  const fileSha256=createHash("sha256").update(bytes).digest("hex");
  if(fileSha256!==producer.public_key.sha256)fail("public key digest");
  if(/-----BEGIN (?:ENCRYPTED |RSA |EC |OPENSSH )?PRIVATE KEY-----/u.test(bytes.toString("ascii")))fail("private key material");
  let keyObject;try{keyObject=createPublicKey(bytes);}catch{fail("public key parse");}
  if(!new Set(["rsa","rsa-pss","ec","ed25519","ed448"]).has(keyObject.asymmetricKeyType))fail("public key type");
  const spki=keyObject.export({type:"spki",format:"der"});
  const spkiSha256=createHash("sha256").update(spki).digest("hex");
  if(spkiSha256!==producer.public_key.spki_sha256)fail("SPKI digest");
  return Object.freeze({path:resolved,bytes:Buffer.from(bytes),fileSha256,spkiSha256,keyObject});
}
