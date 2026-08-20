import { createHash } from "node:crypto";
import { createReadStream } from "node:fs";
import { lstat, readFile, readdir, realpath } from "node:fs/promises";
import path from "node:path";

const JSON_LIMIT = 16n * 1024n * 1024n;
const ARTIFACT_LIMIT = 1024n * 1024n * 1024n * 1024n;
const LEAF = /^[A-Za-z0-9._-]{1,255}$/;
function fail(message) { throw new Error(`candidate admission failed: ${message}`); }
function safeLeaf(value, name) { if (typeof value !== "string" || !LEAF.test(value) || path.basename(value) !== value || value === "." || value === "..") fail(name); return value; }
function immutable(value) { if(value&&typeof value==="object"&&!Buffer.isBuffer(value)){for(const child of Object.values(value)) immutable(child); Object.freeze(value);} return value; }
function parseUniqueJson(buffer) {
  const source=buffer.toString("utf8"); let index=0, depth=0;
  const ws=()=>{while(/[\t\n\r ]/.test(source[index]??""))index++;};
  const string=()=>{if(source[index]!=="\"")fail("invalid JSON"); const start=index++; while(index<source.length){const c=source[index++]; if(c==="\"") {try{return JSON.parse(source.slice(start,index));}catch{fail("invalid JSON");}} if(c==="\\")index++; else if(c<" ")fail("invalid JSON");} fail("invalid JSON");};
  const value=()=>{ws(); if(++depth>128)fail("JSON nesting"); let result; const c=source[index]; if(c==="{")result=object(); else if(c==="[")result=array(); else if(c==="\"")result=string(); else if(source.startsWith("true",index)){index+=4;result=true;} else if(source.startsWith("false",index)){index+=5;result=false;} else if(source.startsWith("null",index)){index+=4;result=null;} else {const match=source.slice(index).match(/^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/); if(!match)fail("invalid JSON"); index+=match[0].length; result=Number(match[0]); if(!Number.isFinite(result))fail("invalid JSON number");} depth--; return result;};
  const object=()=>{index++; ws(); const out={}, keys=new Set(); if(source[index]==="}"){index++;return out;} for(;;){ws(); const key=string(); if(keys.has(key))fail("duplicate JSON key"); keys.add(key); ws(); if(source[index++]!==":")fail("invalid JSON"); out[key]=value(); ws(); const c=source[index++]; if(c==="}")return out; if(c!==",")fail("invalid JSON");}};
  const array=()=>{index++; ws(); const out=[]; if(source[index]==="]"){index++;return out;} for(;;){out.push(value()); ws(); const c=source[index++]; if(c==="]")return out; if(c!==",")fail("invalid JSON");}};
  const result=value(); ws(); if(index!==source.length)fail("invalid JSON"); return result;
}
async function checkedFile(directory,name,limit){
  const file=path.join(directory,safeLeaf(name,"asset name")); const stat=await lstat(file,{bigint:true}).catch(()=>fail("missing asset"));
  if(!stat.isFile()||stat.isSymbolicLink()||stat.size<1n||stat.size>limit)fail("invalid asset");
  const canonical=await realpath(file); if(path.resolve(canonical).toLowerCase()!==path.resolve(file).toLowerCase())fail("linked asset"); return {file,size:stat.size};
}
async function streamDigest(file){const hash=createHash("sha256"); let size=0n; for await(const chunk of createReadStream(file)){hash.update(chunk); size+=BigInt(chunk.length);} return {sha256:hash.digest("hex"),size};}
function expectedSize(value){if(!Number.isSafeInteger(value)||value<1)fail("artifact size contract"); return BigInt(value);}

export async function admitCandidateAssets({row,candidateDirectory}) {
  if(typeof candidateDirectory!=="string"||!path.isAbsolute(candidateDirectory)||path.normalize(candidateDirectory)!==candidateDirectory||path.parse(candidateDirectory).root===candidateDirectory||Buffer.byteLength(candidateDirectory,"utf8")>131072||/[\u0000-\u001f\u007f]/.test(candidateDirectory)) fail("candidate directory");
  const directoryStat=await lstat(candidateDirectory).catch(()=>fail("candidate directory")); if(!directoryStat.isDirectory()||directoryStat.isSymbolicLink())fail("candidate directory");
  const canonical=await realpath(candidateDirectory); if(path.resolve(canonical).toLowerCase()!==path.resolve(candidateDirectory).toLowerCase())fail("linked directory");
  const names=[row?.artifact?.name,row?.evidence?.name,row?.provenance?.bundle_name].map((name)=>safeLeaf(name,"asset name")); if(new Set(names).size!==3)fail("distinct asset names");
  const entries=await readdir(candidateDirectory,{withFileTypes:true}); if(entries.length!==3||entries.some(entry=>!names.includes(entry.name)||!entry.isFile()||entry.isSymbolicLink()))fail("candidate file set");
  const artifact=await checkedFile(candidateDirectory,names[0],ARTIFACT_LIMIT); if(artifact.size!==expectedSize(row.artifact.size_bytes))fail("artifact size mismatch"); const streamed=await streamDigest(artifact.file); if(streamed.size!==artifact.size)fail("artifact changed during admission"); if(streamed.sha256!==row.artifact.sha256)fail("artifact digest mismatch");
  const evidence=await checkedFile(candidateDirectory,names[1],JSON_LIMIT); const evidenceBytes=await readFile(evidence.file); const evidenceSha256=createHash("sha256").update(evidenceBytes).digest("hex"); if(evidenceSha256!==row.evidence.sha256)fail("evidence digest mismatch"); const evidenceJson=parseUniqueJson(evidenceBytes);
  const bundle=await checkedFile(candidateDirectory,names[2],JSON_LIMIT); const bundleBytes=await readFile(bundle.file); const bundleSha256=createHash("sha256").update(bundleBytes).digest("hex"); if(bundleSha256!==row.provenance.bundle_sha256)fail("bundle digest mismatch"); const bundleJson=parseUniqueJson(bundleBytes);
  return immutable({directory:candidateDirectory,artifact:{path:artifact.file,sizeBytes:Number(artifact.size),sha256:streamed.sha256},evidence:{path:evidence.file,bytes:Buffer.from(evidenceBytes),json:evidenceJson,sizeBytes:Number(evidence.size),sha256:evidenceSha256},bundle:{path:bundle.file,bytes:Buffer.from(bundleBytes),json:bundleJson,sizeBytes:Number(bundle.size),sha256:bundleSha256}});
}
