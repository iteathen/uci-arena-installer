import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {existsSync, mkdirSync, readFileSync, statSync, writeFileSync} from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import {ownedTestTemp} from './test-support/owned-temp.mjs';

// Opt-in native CLI tests use only fresh owned fixture roots and no daemon.
const executable = process.env.ARENA_SETUP_EXECUTABLE;
test('public locator clear is idempotent and rejects conflicting native CLI actions', {
  skip: !executable,
}, t => {
  assert.ok(path.isAbsolute(executable));
  const root = ownedTestTemp(t, 'arena-locator-cli-');
  const data = path.join(root, 'data');
  const run = args => spawnSync(executable, [
    '--json', '--install-root', path.join(root, 'install'), '--data-root', data,
    '--cache-root', path.join(root, 'cache'), '--workspace-root', path.join(root, 'workspace'),
    '--knowledge-root', path.join(root, 'knowledge'), 'import', ...args,
  ], {encoding: 'utf8', windowsHide: true, timeout: 10000});
  const first = run(['--kind', 'uci_engine', '--clear']);
  assert.equal(first.status, 0, first.stderr);
  assert.deepEqual(JSON.parse(first.stdout), {
    schema: 'arena_setup_import_result_v1', schema_version: 1, kind: 'uci_engine',
    mode: 'clear_saved_locator', state: 'cleared', removed: false, refreshed_consumers: [],
  });
  const locators = path.join(data, 'locators');
  mkdirSync(locators, {recursive: true});
  const locator = path.join(locators, 'uci_engine.json');
  const fixtureBytes = '{"historical":"retained fixture"}';
  writeFileSync(locator, fixtureBytes);
  for (const args of [
    ['--kind', 'uci_engine', '--clear', '--path', root],
    ['--kind', 'uci_engine', '--clear', '--managed-copy'],
    ['--kind', 'opening_book', '--clear', '--authority-mode', 'immutable_pinned_snapshot'],
    ['--kind', 'unknown', '--clear'], ['--kind', 'restaurant', '--clear'],
    ['--kind', 'uci_engine'],
  ]) {
    const failed = run(args);
    assert.notEqual(failed.status, 0, failed.stdout);
    assert.equal(readFileSync(locator, 'utf8'), fixtureBytes);
  }
  const disabledClear = run(['--kind', 'uci_engine', '--clear=false']);
  assert.notEqual(disabledClear.status, 0, disabledClear.stdout);
  assert.match(disabledClear.stderr, /import requires a path or explicit clear/u);
  assert.equal(readFileSync(locator, 'utf8'), fixtureBytes);
  const cleared = run(['--kind', 'uci_engine', '--clear']);
  assert.equal(cleared.status, 0, cleared.stderr);
  assert.equal(JSON.parse(cleared.stdout).removed, true);
  assert.equal(existsSync(locator), false);
  const repeated = run(['--kind', 'uci_engine', '--clear']);
  assert.equal(repeated.status, 0, repeated.stderr);
  assert.equal(JSON.parse(repeated.stdout).removed, false);
  mkdirSync(locator);
  const wrongKind = run(['--kind', 'uci_engine', '--clear']);
  assert.notEqual(wrongKind.status, 0, wrongKind.stdout);
  assert.equal(statSync(locator).isDirectory(), true);
});
