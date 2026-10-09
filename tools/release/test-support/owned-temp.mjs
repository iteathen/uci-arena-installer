import assert from 'node:assert/strict';
import {lstatSync, mkdtempSync, realpathSync, rmSync} from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Only fresh test directories are owned. Check the actual incarnation just
// before recursive deletion; a reused name or reparse target is not ours.
export function ownedTestTemp(t, prefix) {
  assert.match(prefix, /^arena-[a-z0-9-]+-$/u);
  const parent = realpathSync(os.tmpdir());
  const root = mkdtempSync(path.join(parent, prefix));
  const name = path.basename(root);
  const original = lstatSync(root, {bigint: true});
  const fold = value => process.platform === 'win32' ? value.toLowerCase() : value;
  t.after(() => {
    assert.equal(lstatSync(parent).isSymbolicLink(), false);
    assert.equal(fold(realpathSync(parent)), fold(parent));
    const current = lstatSync(root, {bigint: true});
    assert.equal(current.isSymbolicLink(), false);
    assert.equal(current.isDirectory(), true);
    const actual = realpathSync(root);
    assert.equal(fold(actual), fold(root));
    assert.equal(fold(path.dirname(actual)), fold(parent));
    assert.equal(path.basename(actual), name);
    assert.ok(name.startsWith(prefix) && name.length > prefix.length);
    assert.deepEqual([current.dev, current.ino, current.birthtimeNs],
      [original.dev, original.ino, original.birthtimeNs]);
    rmSync(actual, {recursive: true, force: true});
  });
  return root;
}
