import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import path from "node:path";
import process from "node:process";
import test from "node:test";

const repositoryRoot = path.resolve(import.meta.dirname, "../..");

test("release build CLIs execute when invoked through relative Windows-safe paths", () => {
  for (const script of [
    "tools/release/build-installer-runtime.mjs",
    "tools/release/build-manager-linux-package.mjs",
    "tools/release/build-manager-msi.mjs",
    "tools/release/build-windows-suite-bundle.mjs",
  ]) {
    const result = spawnSync(process.execPath, [script], {
      cwd: repositoryRoot,
      encoding: "utf8",
      windowsHide: true,
    });
    assert.notEqual(result.status, 0, `${script} silently skipped its CLI entrypoint`);
    assert.match(result.stderr, /missing --/u, `${script} did not validate its invocation`);
  }
});
