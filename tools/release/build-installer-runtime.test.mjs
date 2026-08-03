import assert from "node:assert/strict";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  buildInstallerRuntime,
  validateInstallerRuntime,
} from "./build-installer-runtime.mjs";

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-installer-runtime-"));
  const installTree = path.join(root, "install tree");
  mkdirSync(path.join(installTree, "bin"), { recursive: true });
  mkdirSync(path.join(installTree, "share", "licenses"), { recursive: true });
  writeFileSync(path.join(installTree, "bin", "arena-setup.exe"), "MZsetup");
  writeFileSync(path.join(installTree, "bin", "archive.dll"), "runtime");
  writeFileSync(path.join(installTree, "share", "licenses", "LICENSE"), "license");
  return { root, installTree, output: path.join(root, "runtime output") };
}

test("Installer runtime binds the complete staged dependency closure", () => {
  const f = fixture();
  try {
    const built = buildInstallerRuntime({
      installTree: f.installTree,
      outputRoot: f.output,
      version: "0.1.0",
      target: "windows-x86_64",
      sourceDateEpoch: 1_700_000_000,
    });
    assert.deepEqual([...built.declared.keys()], [
      "bin/archive.dll",
      "bin/arena-setup.exe",
      "share/licenses/LICENSE",
    ]);
    assert.equal(built.entrypoint, "bin/arena-setup.exe");
    assert.equal(
      JSON.parse(readFileSync(built.manifestPath, "utf8")).files.length,
      3,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Installer runtime rejects undeclared bytes and overlapping roots", () => {
  const f = fixture();
  try {
    buildInstallerRuntime({
      installTree: f.installTree,
      outputRoot: f.output,
      version: "0.1.0",
      target: "windows-x86_64",
      sourceDateEpoch: 1_700_000_000,
    });
    writeFileSync(path.join(f.output, "unexpected.dll"), "stale");
    assert.throws(
      () => validateInstallerRuntime({
        root: f.output,
        version: "0.1.0",
        target: "windows-x86_64",
      }),
      /inventory mismatch/u,
    );
    assert.throws(
      () => buildInstallerRuntime({
        installTree: f.installTree,
        outputRoot: path.join(f.installTree, "nested"),
        version: "0.1.0",
        target: "windows-x86_64",
        sourceDateEpoch: 1_700_000_000,
      }),
      /must not overlap/u,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});
