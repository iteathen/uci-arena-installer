import assert from "node:assert/strict";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  symlinkSync,
  unlinkSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import {ownedTestTemp} from './test-support/owned-temp.mjs';

import {
  cleanBuildArtifactNames,
  renameWithTransientRetry,
  resolveNpmInvocation,
  selectLockedComponents,
  transactionalOutput,
  removeOwnedTree,
  validateWindowsLauncherEnvelope,
} from "./clean-build.mjs";

test("clean build retries transient Windows rename contention with bounded backoff", () => {
  const waits = [];
  let calls = 0;
  renameWithTransientRetry("staged", "published", {
    attempts: 4,
    rename: () => {
      calls += 1;
      if (calls < 3) {
        const error = new Error("synthetic scanner contention");
        error.code = "EPERM";
        throw error;
      }
    },
    wait: (milliseconds) => waits.push(milliseconds),
  });
  assert.equal(calls, 3);
  assert.deepEqual(waits, [50, 100]);

  let nonTransientCalls = 0;
  assert.throws(() => renameWithTransientRetry("staged", "published", {
    rename: () => {
      nonTransientCalls += 1;
      const error = new Error("synthetic invalid target");
      error.code = "EINVAL";
      throw error;
    },
    wait: () => assert.fail("non-transient errors must not be delayed"),
  }), /synthetic invalid target/u);
  assert.equal(nonTransientCalls, 1);
});

test("clean build invokes npm through Node on Windows without shell parsing", () => {
  const node = String.raw`C:\Program Files\nodejs\node.exe`;
  const npmCli = String.raw`C:\Program Files\nodejs\node_modules\npm\bin\npm-cli.js`;
  assert.deepEqual(resolveNpmInvocation({
    platform: "win32",
    execPath: node,
    environment: {},
    fileExists: (candidate) => candidate === npmCli,
  }), {
    executable: node,
    argsPrefix: [npmCli],
  });
  assert.deepEqual(resolveNpmInvocation({ platform: "linux" }), {
    executable: "npm",
    argsPrefix: [],
  });
  assert.throws(() => resolveNpmInvocation({
    platform: "win32",
    execPath: String.raw`C:\portable\node.exe`,
    environment: { PATH: String.raw`C:\unrelated` },
    fileExists: () => false,
  }), /npm CLI is unavailable/u);
});

test("clean build publishes the complete platform-specific output contract", () => {
  assert.deepEqual(cleanBuildArtifactNames("windows-x86_64", "0.3.9"), [
    "UCI-Arena-Setup-0.3.9-windows-x64-online.exe",
    "UCI-Arena-Setup-0.3.9-windows-x64-offline.exe",
    "UCI-Arena-Manager-0.3.9-windows-x64-user.msi",
    "UCI-Arena-Manager-0.3.9-windows-x64-machine.msi",
  ]);
  assert.deepEqual(cleanBuildArtifactNames("linux-x86_64", "0.3.9"), [
    "uci-arena-setup-0.3.9-linux-x86_64-online.run",
    "uci-arena-setup-0.3.9-linux-x86_64-offline.run",
    "uci-arena-manager_0.3.9_amd64.deb",
    "uci-arena-manager-0.3.9-1.x86_64.rpm",
    "uci-arena-manager-0.3.9-linux-x86_64.tar.gz",
  ]);
  assert.throws(
    () => cleanBuildArtifactNames("linux-x86_64", "manager-version"),
    /three-field suite version/u,
  );
});

test("clean build validates each assembled Windows envelope without installed-scope policy", () => {
  const calls = [];
  validateWindowsLauncherEnvelope(String.raw`C:\candidate\setup.exe`, {
    execute: (filename, args, options) => {
      calls.push({ filename, args, options });
      return { status: 0, stdout: "", stderr: "" };
    },
  });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].filename, String.raw`C:\candidate\setup.exe`);
  assert.deepEqual(calls[0].args, ["--validate-envelope"]);
  assert.equal(calls[0].options.timeout, 120_000);
  validateWindowsLauncherEnvelope(String.raw`C:\candidate\setup.exe`, {
    timeoutMilliseconds: 45_000,
    execute: (filename, args, options) => {
      calls.push({ filename, args, options });
      return { status: 0, stdout: "", stderr: "" };
    },
  });
  assert.equal(calls[1].options.timeout, 45_000);
  assert.throws(
    () => validateWindowsLauncherEnvelope(String.raw`C:\candidate\setup.exe`, {
      timeoutMilliseconds: 0,
    }),
    /envelope-validation timeout policy is invalid/u,
  );
  assert.throws(
    () => validateWindowsLauncherEnvelope(String.raw`C:\candidate\setup.exe`, {
      execute: () => ({ status: -1073741571, stdout: "", stderr: "" }),
    }),
    /envelope validation failed: exit -1073741571/u,
  );
});

test("clean build selects one exact target or portable row per composed component", () => {
  const composition = {
    components: [
      { component_id: "manager.core", repository: "example/manager" },
      { component_id: "node.private", repository: "iteathen/uci-arena-installer" },
    ],
  };
  const lock = {
    components: [{ component_id: "manager.core", target_triple: "windows-x86_64" }],
    managed_components: [{ component_id: "node.private", target_triple: "any-any" }],
  };
  const selected = selectLockedComponents(lock, composition, "windows-x86_64");
  assert.equal(selected.length, 2);
  assert.equal(selected[0].managed, false);
  assert.equal(selected[1].managed, true);
  lock.components.push({ component_id: "manager.core", target_triple: "any-any" });
  assert.throws(
    () => selectLockedComponents(lock, composition, "windows-x86_64"),
    /has 2 candidates/u,
  );
});

test("clean build transaction preserves previous complete output on failure", (t) => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-clean-build-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const output = path.join(root, "dist");
  transactionalOutput(output, ({ publishRoot }) => {
    writeFileSync(path.join(publishRoot, "accepted.exe"), "accepted");
    return "accepted";
  });
  assert.equal(readFileSync(path.join(output, "accepted.exe"), "utf8"), "accepted");
  assert.throws(() => transactionalOutput(output, ({ publishRoot }) => {
    writeFileSync(path.join(publishRoot, "partial.exe"), "partial");
    throw new Error("synthetic package failure");
  }), /synthetic package failure/u);
  assert.equal(readFileSync(path.join(output, "accepted.exe"), "utf8"), "accepted");
  assert.equal(existsSync(path.join(output, "partial.exe")), false);
});

test("v3 selection uses unified ownership rows without a legacy managed split", () => {
  const composition={components:[{component_id:'node.private',repository:'iteathen/uci-arena-installer'},
    {component_id:'arbitrary.provider',repository:'owner/arbitrary'}]};
  const lock={schema:'arena_suite_release_lock_v3',components:[
    {component_id:'node.private',target_triple:'windows-x86_64',ownership:'installer_managed'},
    {component_id:'arbitrary.provider',target_triple:'any',ownership:'provider'},
  ]};
  const selected=selectLockedComponents(lock,composition,'windows-x86_64');
  assert.deepEqual(selected.map(row=>[row.lockRow.component_id,row.managed]),
    [['node.private',true],['arbitrary.provider',false]]);
});

test("output publication waits for asynchronous verification and preserves previous output on late failure", async (t) => {
  const root=ownedTestTemp(t,'arena-awaited-output-');
  const output=path.join(root,'dist');
  transactionalOutput(output,({publishRoot})=>writeFileSync(path.join(publishRoot,'old'),'accepted'));
  let release;
  const pending=new Promise(resolve=>{release=resolve;});
  const operation=transactionalOutput(output,async({publishRoot})=>{
    writeFileSync(path.join(publishRoot,'new'),'candidate');await pending;throw Error('late verifier failure');
  });
  // Attach rejection immediately; it must not turn into an unhandled rejection.
  const failure=assert.rejects(Promise.resolve(operation),/late verifier failure/);
  try {assert.equal(readFileSync(path.join(output,'old'),'utf8'),'accepted');
    assert.equal(existsSync(path.join(output,'new')),false);} finally {release();}
  await failure;
  assert.equal(readFileSync(path.join(output,'old'),'utf8'),'accepted');
});

test('owned cleanup rejects outside roots and reparse targets without deleting foreign content',t=>{
  const root=ownedTestTemp(t,'arena-contained-cleanup-');
  const owner=path.join(root,'owner'),outside=path.join(root,'outside');mkdirSync(owner);mkdirSync(outside);
  writeFileSync(path.join(outside,'retain'),'foreign');
  assert.throws(()=>removeOwnedTree(owner,outside),/escapes/);
  assert.throws(()=>removeOwnedTree(owner,owner),/escapes/);
  const link=path.join(owner,'alias');
  try{symlinkSync(outside,link,process.platform==='win32'?'junction':'dir');}
  catch(error){if(error.code==='EPERM'){t.skip('platform does not permit test reparse creation');return;}throw error;}
  assert.throws(()=>removeOwnedTree(owner,link),/linked/);
  assert.equal(readFileSync(path.join(outside,'retain'),'utf8'),'foreign');
});

test('failed publication and rollback retain previous bytes and first publication failure', t => {
  const root = ownedTestTemp(t, 'arena-rollback-retention-');
  const output = path.join(root, 'dist');
  transactionalOutput(output, ({ publishRoot }) => writeFileSync(path.join(publishRoot, 'old'), 'accepted'));
  const publicationError = Error('publication rename failed');
  const rollbackError = Error('rollback rename blocked');
  let failure;
  try { transactionalOutput(output, ({ publishRoot }) => {
    writeFileSync(path.join(publishRoot, 'new'), 'candidate');
  }, { rename: (source, target) => {
    if (path.basename(source) === 'publish') throw publicationError;
    if (path.basename(source) === 'previous') throw rollbackError;
    renameSync(source, target);
  } }); } catch (error) { failure = error; }
  assert.equal(existsSync(output), false);
  const transactions = readdirSync(root).filter(name => name.startsWith('.dist.transaction-'));
  assert.equal(transactions.length, 1);
  assert.equal(readFileSync(path.join(root, transactions[0], 'previous', 'old'), 'utf8'), 'accepted');
  assert.equal(readFileSync(path.join(root, transactions[0], 'publish', 'new'), 'utf8'), 'candidate');
  assert.ok(failure instanceof AggregateError);
  assert.equal(failure.cause, publicationError);
  assert.deepEqual(failure.errors, [publicationError, rollbackError]);
  assert.match(failure.message, /rollback failed; retained/u);
});

test('failed publication restores prior output and cleans only its completed transaction', t => {
  const root = ownedTestTemp(t, 'arena-restored-output-');
  const output = path.join(root, 'dist');
  transactionalOutput(output, ({ publishRoot }) => writeFileSync(path.join(publishRoot, 'old'), 'accepted'));
  const publicationError = Error('publication rename failed');
  assert.throws(() => transactionalOutput(output, ({ publishRoot }) => {
    writeFileSync(path.join(publishRoot, 'new'), 'candidate');
  }, { rename: (source, target) => {
    if (path.basename(source) === 'publish') throw publicationError;
    renameSync(source, target);
  } }), error => error === publicationError);
  assert.equal(readFileSync(path.join(output, 'old'), 'utf8'), 'accepted');
  assert.deepEqual(readdirSync(root), ['dist']);
});

test('test cleanup rejects a replaced root without following its foreign target', t => {
  const outside = ownedTestTemp(t, 'arena-test-cleanup-foreign-');
  writeFileSync(path.join(outside, 'retain'), 'foreign');
  let cleanup;
  const root = ownedTestTemp({after: callback => { cleanup = callback; }}, 'arena-test-cleanup-owned-');
  cleanup(); // Delete only the original, verified test-owned incarnation.
  try { symlinkSync(outside, root, process.platform === 'win32' ? 'junction' : 'dir'); }
  catch (error) {
    if (error.code === 'EPERM') { t.skip('platform does not permit test reparse creation'); return; }
    throw error;
  }
  t.after(() => unlinkSync(root)); // Unlink the test-created alias, never recurse.
  assert.throws(cleanup, /Expected values to be strictly equal/u);
  assert.equal(readFileSync(path.join(outside, 'retain'), 'utf8'), 'foreign');
});
