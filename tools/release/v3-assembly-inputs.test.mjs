import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {copyFileSync,mkdirSync,readFileSync,writeFileSync,existsSync} from 'node:fs';
import {createHash} from 'node:crypto';
import path from 'node:path';
import test from 'node:test';
import {loadReleaseInputs,loadReleaseValidationOptions} from './release-lock.mjs';
import {verifyAndExtractCandidates,verifyV3Candidates} from './clean-build.mjs';
import {buildSuiteCatalog} from './suite-catalog.mjs';
import {ownedTestTemp} from './test-support/owned-temp.mjs';

// Optional real-data verification, never fixture qualification or assembly.
const inputRoot=process.env.ARENA_V3_REAL_INPUTS;
test('public assembly CLI verifies eight exact original signed v3 inputs without extraction or build',{
  skip:!inputRoot,
},()=>{
  const r=spawnSync(process.execPath,['tools/release/clean-build.mjs','--complete-development',
    '--validate-inputs','--platform','windows-x86_64','--validation-mode','official',
    '--lock',path.join(inputRoot,'release-lock.json'),'--composition-directory',inputRoot,
    '--trust-policy',path.join(inputRoot,'provenance-trust-policy.json'),
    '--candidate-directories',path.join(inputRoot,'candidate-directories.json')],{
    cwd:path.resolve(import.meta.dirname,'../..'),encoding:'utf8',timeout:180000,windowsHide:true,
  });
  assert.equal(r.status,0,r.stderr);const result=JSON.parse(r.stdout);
  assert.equal(result.schema,'arena_suite_inputs_verified_v1');assert.equal(result.components.length,8);
  const lock=JSON.parse(readFileSync(path.join(inputRoot,'release-lock.json')));
  const policyBytes=readFileSync(path.join(inputRoot,lock.trust_policy.name));
  assert.equal(result.trust_policy_sha256,lock.trust_policy.sha256);
  assert.equal(createHash('sha256').update(policyBytes).digest('hex'),lock.trust_policy.sha256);
  assert(result.components.every(c=>c.schema==='arena_verified_component_provenance_v1'));
  assert.equal(result.built,false);assert.equal(result.extracted,false);
  assert.deepEqual(result.components.map(c=>c.component_id).sort(),lock.components.map(c=>c.component_id).sort());
});

test('real v3 policy and last-row byte failures reject before any extraction or build',{
  skip:!inputRoot,
},async t=>{
  const root=ownedTestTemp(t,'arena-v3-falsifier-');
  const inputs=loadReleaseInputs(path.join(inputRoot,'release-lock.json'),inputRoot);
  const policyPath=path.join(inputRoot,'provenance-trust-policy.json');
  const policyCopy=path.join(root,'provenance-trust-policy.json');
  writeFileSync(policyCopy,readFileSync(policyPath).toString()+' ');
  assert.throws(()=>loadReleaseValidationOptions(inputs,{trustPolicyFilename:policyCopy,
    validationMode:'official',requiredTargets:['windows-x86_64']}),/digest/);
  assert.throws(()=>loadReleaseValidationOptions(inputs,{trustPolicyFilename:policyPath,
    requiredTargets:['windows-x86_64']}),/validation-mode/);
  const options=loadReleaseValidationOptions(inputs,{trustPolicyFilename:policyPath,
    validationMode:'official',requiredTargets:['windows-x86_64']});
  const map=JSON.parse(readFileSync(path.join(inputRoot,'candidate-directories.json')));
  const row=inputs.lock.components.at(-1),changed=path.join(root,'changed');mkdirSync(changed);
  for(const name of [row.artifact.name,row.evidence.name,row.provenance.bundle_name])copyFileSync(path.join(map[row.component_id],name),path.join(changed,name));
  writeFileSync(path.join(changed,row.provenance.bundle_name),readFileSync(path.join(changed,row.provenance.bundle_name)).toString()+' ');
  map[row.component_id]=changed;const workRoot=path.join(root,'work');mkdirSync(workRoot);
  await assert.rejects(verifyAndExtractCandidates({lock:inputs.lock,composition:inputs.composition,
    target:'windows-x86_64',candidateDirectories:map,validationOptions:options,
    trustPolicyFilename:policyPath,workRoot,cosignExecutable:process.env.COSIGN_EXECUTABLE}),/bundle digest/);
  assert.equal(existsSync(path.join(workRoot,'extracted')),false);
});

test('all eight genuine verified v3 archives close manifest and catalog dependencies before packaging',{
  skip:!inputRoot,
},async t=>{
  const root=ownedTestTemp(t,'arena-v3-archives-');
  const inputs=loadReleaseInputs(path.join(inputRoot,'release-lock.json'),inputRoot);
  const trustPolicyFilename=path.join(inputRoot,'provenance-trust-policy.json');
  const validationOptions=loadReleaseValidationOptions(inputs,{trustPolicyFilename,
    validationMode:'official',requiredTargets:['windows-x86_64']});
  const candidates=await verifyAndExtractCandidates({lock:inputs.lock,composition:inputs.composition,
    target:'windows-x86_64',candidateDirectories:JSON.parse(readFileSync(path.join(inputRoot,'candidate-directories.json'))),
    validationOptions,trustPolicyFilename,workRoot:root,cosignExecutable:process.env.COSIGN_EXECUTABLE});
  assert.equal(candidates.length,8);
  assert(candidates.every(c=>c.verified.schema==='arena_verified_component_provenance_v1'&&!Object.hasOwn(c,'providerResult')));
  const catalog=buildSuiteCatalog({composition:inputs.composition,target:'windows-x86_64',candidates,
    featureComponentIds:['uci_arena.vector','book_forge.service','syzygy.root-provider','uci_arena_evidence.service','lichess.adapter']});
  assert.equal(catalog.components.length,8);
  assert.deepEqual(catalog.components.find(c=>c.id==='uci_arena.vector').dependencies,['node_runtime.private']);
});

test('v3 policy loader requires exact file bytes and explicit mode before transport',async t=>{
  const root=ownedTestTemp(t,'arena-v3-policy-');
  const filename=path.join(root,'policy.json');const bytes=Buffer.from('{}');writeFileSync(filename,bytes);
  const inputs={lock:{schema:'arena_suite_release_lock_v3',trust_policy:{name:'policy.json',sha256:'a'.repeat(64)}}};
  assert.throws(()=>loadReleaseValidationOptions(inputs,{trustPolicyFilename:filename}),/validation-mode/);
  assert.throws(()=>loadReleaseValidationOptions(inputs,{trustPolicyFilename:filename,validationMode:'official'}),/digest/);
  assert.throws(()=>loadReleaseValidationOptions(inputs,{validationMode:'official'}),/trust-policy/);
  // A caller-supplied parsed policy cannot bypass reading and binding actual bytes.
  await assert.rejects(verifyV3Candidates({lock:inputs.lock,composition:{},target:'windows-x86_64',
    validationOptions:{validationMode:'official',trustPolicy:{}},trustPolicyFilename:filename}),/digest/);
});
