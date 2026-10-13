import assert from 'node:assert/strict';
import {cpSync,mkdirSync,writeFileSync,existsSync} from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import test from 'node:test';
import {ownedTestTemp} from './test-support/owned-temp.mjs';

// Optional real v3 lock input; no crypto, extraction, build or live action.
const inputRoot=process.env.ARENA_V3_UPDATE_CLI_INPUTS;
test('public update CLI reads a declared candidate directory document before verification', {skip:!inputRoot}, t=>{
  const root=ownedTestTemp(t,'arena-update-cli-map-');
  const repository=path.join(root,'source');mkdirSync(repository);
  const release=path.join(repository,'tools/release');
  cpSync(import.meta.dirname,release,{recursive:true});
  writeFileSync(path.join(repository,'package.json'),' {"type":"module"}\n');
  const emptyTemplate=path.join(root,'empty-git-template');mkdirSync(emptyTemplate);
  const git=args=>{const r=spawnSync('git',args,{cwd:repository,encoding:'utf8',windowsHide:true});assert.equal(r.status,0,r.stderr);};
  git(['init','--template='+emptyTemplate]);git(['add','.']);
  git(['-c','user.name=CLI Fixture','-c','user.email=fixture@example.invalid','commit','-m','Exact copied source fixture']);
  const map=path.join(root,'invalid-candidate-map.json');writeFileSync(map,'{invalid JSON');
  const output=path.join(root,'not-published');
  const result=spawnSync(process.execPath,[path.join(release,'build-suite-update-package.mjs'),
    '--platform','windows-x86_64','--component','book_forge.service','--diagnostic-review',
    '--validation-mode','diagnostic_review','--lock',path.join(inputRoot,'release-lock.json'),
    '--composition-directory',inputRoot,'--trust-policy',path.join(inputRoot,'provenance-trust-policy.json'),
    '--candidate-directories',map,'--output-root',output],{cwd:repository,encoding:'utf8',timeout:30000,windowsHide:true});
  assert.equal(result.status,1,result.stderr);
  assert.match(result.stderr,/JSON|Unexpected token|Expected property/u);
  assert.doesNotMatch(result.stderr,/readFileSync is not defined|clean tracked worktree/u);
  assert.equal(existsSync(output),false);
});
