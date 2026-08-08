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
  cleanBuildArtifactNames,
  renameWithTransientRetry,
  resolveNpmInvocation,
  selectLockedComponents,
  transactionalOutput,
  validateWindowsLauncherExecutable,
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

test("clean build executes each assembled Windows launcher before publication", () => {
  const calls = [];
  validateWindowsLauncherExecutable(String.raw`C:\candidate\setup.exe`, {
    scope: "all-users",
    execute: (filename, args, options) => {
      calls.push({ filename, args, options });
      return { status: 0, stdout: "", stderr: "" };
    },
  });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].filename, String.raw`C:\candidate\setup.exe`);
  assert.deepEqual(calls[0].args, ["--dry-run", "--scope", "all-users"]);
  assert.equal(calls[0].options.timeout, 120_000);
  validateWindowsLauncherExecutable(String.raw`C:\candidate\setup.exe`, {
    timeoutMilliseconds: 45_000,
    execute: (filename, args, options) => {
      calls.push({ filename, args, options });
      return { status: 0, stdout: "", stderr: "" };
    },
  });
  assert.equal(calls[1].options.timeout, 45_000);
  assert.throws(
    () => validateWindowsLauncherExecutable(String.raw`C:\candidate\setup.exe`, {
      timeoutMilliseconds: 0,
    }),
    /timeout policy is invalid/u,
  );
  assert.throws(
    () => validateWindowsLauncherExecutable(String.raw`C:\candidate\setup.exe`, {
      execute: () => ({ status: -1073741571, stdout: "", stderr: "" }),
    }),
    /dry-run failed for personal: exit -1073741571/u,
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
