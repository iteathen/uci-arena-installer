import assert from "node:assert/strict";
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  selectLockedComponents,
  transactionalOutput,
} from "./clean-build.mjs";

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
