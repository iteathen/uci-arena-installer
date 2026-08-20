import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { resolveCosignExecutable, runCosign } from "./cosign-cli.mjs";

test("Cosign resolution accepts only a regular explicit override",()=>{const root=mkdtempSync(path.join(os.tmpdir(),"cosign-"));try{const file=path.join(root,"cosign.bin");writeFileSync(file,"fixture");assert.equal(resolveCosignExecutable({override:file}),path.resolve(file));assert.throws(()=>resolveCosignExecutable({override:root}));}finally{rmSync(root,{recursive:true,force:true});}});
test("Cosign resolution preserves the bounded system fallback",()=>{assert.equal(resolveCosignExecutable({env:{},platform:process.platform,arch:process.arch}),"cosign");});
test("Cosign execution rejects unsafe arguments and unsupported version output",()=>{assert.throws(()=>runCosign({cosignPath:process.execPath,args:["verify\nunsafe"]}));assert.throws(()=>runCosign({cosignPath:process.execPath,args:["verify-blob-attestation"],timeoutMs:5000,maxOutputBytes:4096}),/version|Cosign command/u);});
test("Cosign execution enforces timeout and output bounds",()=>{assert.throws(()=>runCosign({cosignPath:"cosign",args:[],timeoutMs:0,maxOutputBytes:1}));assert.throws(()=>runCosign({cosignPath:"cosign",args:[],timeoutMs:1,maxOutputBytes:20*1024*1024}));});
