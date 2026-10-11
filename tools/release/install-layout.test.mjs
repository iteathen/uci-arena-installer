import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

import {
  buildWindowsWixLayoutProjection,
  resolveInstallLayout,
  validateInstallLayoutPolicy,
} from "./install-layout.mjs";

const root = path.resolve(import.meta.dirname, "../..");
const policy = JSON.parse(readFileSync(path.join(root, "packaging/install-layout.json"), "utf8"));
const anchors = {
  user_local_app_data: "C:\\Users\\Example\\AppData\\Local",
  machine_program_files: "C:\\Program Files",
  machine_program_data: "C:\\ProgramData",
};
const clone = (value = policy) => structuredClone(value);

test("an explicit fresh package selection derives every child on the selected volumes", () => {
  const selectedRoots = { integration: "E:\\Programs\\UCI Arena Manager", data: "E:\\Data\\UCI Arena Manager" };
  const selected = resolveInstallLayout({ policy, scope: "per-user", anchors, selectedRoots });
  assert.equal(selected.integration_root, "E:\\Programs\\UCI Arena Manager");
  assert.deepEqual(selected.roots, {
    data: "E:\\Data\\UCI Arena Manager",
    install: "E:\\Programs\\UCI Arena Manager\\installed",
    cache: "E:\\Data\\UCI Arena Manager\\cache",
    workspace: "E:\\Data\\UCI Arena Manager\\workspaces",
    knowledge: "E:\\Data\\UCI Arena Manager\\knowledge",
    release: "E:\\Data\\UCI Arena Manager\\release",
    receipts: "E:\\Data\\UCI Arena Manager\\receipts",
    transactions: "E:\\Data\\UCI Arena Manager\\transactions",
    preservation: "E:\\Data\\UCI Arena Manager\\preservation",
  });
});

test("fresh package selection rejects partial unsafe and overlapping roots", () => {
  for (const selectedRoots of [
    { integration: "E:\\Programs\\Arena" },
    { integration: "E:\\", data: "E:\\Data\\Arena" },
    { integration: "relative", data: "E:\\Data\\Arena" },
    { integration: "E:\\Programs\\Arena", data: "e:\\programs\\arena\\data" },
    { integration: "E:\\Programs\\Arena", data: "E:\\Data\\Arena", extra: true },
  ]) assert.throws(() => resolveInstallLayout({ policy, scope: "per-user", anchors, selectedRoots }), TypeError);
});

test("active install-layout policy resolves exact personal and machine targets", () => {
  const personal = resolveInstallLayout({ policy, scope: "per-user", anchors });
  assert.equal(personal.integration_root, "C:\\Users\\Example\\AppData\\Local\\Programs\\UCI Arena Manager");
  assert.equal(personal.roots.install, `${personal.integration_root}\\installed`);
  assert.equal(personal.roots.data, "C:\\Users\\Example\\AppData\\Local\\UCI Arena Manager");
  assert.equal(personal.roots.transactions, `${personal.roots.data}\\transactions`);
  assert.equal(personal.stable_paths.setup_launcher, `${personal.integration_root}\\bin\\arena-setup.exe`);
  assert.equal(personal.registration.hive, "HKCU");

  const machine = resolveInstallLayout({ policy, scope: "per-machine", anchors });
  assert.equal(machine.integration_root, "C:\\Program Files\\UCI Arena Manager");
  assert.equal(machine.roots.data, "C:\\ProgramData\\UCI Arena Manager");
  assert.equal(machine.roots.install, "C:\\Program Files\\UCI Arena Manager\\installed");
  assert.equal(machine.stable_paths.policy, "C:\\Program Files\\UCI Arena Manager\\share\\uci-arena\\layout-policies\\arena_windows_standard-v2.json");
  assert.equal(machine.registration.hive, "HKLM");
});

test("validation and projections are immutable deterministic clones", () => {
  const input = clone();
  const before = structuredClone(input);
  const validated = validateInstallLayoutPolicy(input);
  assert.deepEqual(input, before);
  assert.notEqual(validated, input);
  assert.equal(Object.isFrozen(validated.scopes["per-user"].roots), true);
  const first = buildWindowsWixLayoutProjection({ policy: input, scope: "per-user" });
  const second = buildWindowsWixLayoutProjection({ policy: input, scope: "per-user" });
  assert.deepEqual(first, second);
  assert.equal(JSON.stringify(first, null, 2), JSON.stringify(second, null, 2));
});

test("WiX projection uses exact semantic folder mappings and expressions", () => {
  const personal = buildWindowsWixLayoutProjection({ policy, scope: "per-user" });
  assert.deepEqual(personal.anchors.integration, {
    semantic_anchor: "user_local_app_data",
    wix_folder_id: "LocalAppDataFolder",
    segments: ["Programs", "UCI Arena Manager"],
    expression: "[LocalAppDataFolder]Programs\\UCI Arena Manager",
  });
  assert.equal(personal.roots.install.expression, "installed");
  const machine = buildWindowsWixLayoutProjection({ policy, scope: "per-machine" });
  assert.equal(machine.anchors.integration.wix_folder_id, "ProgramFiles64Folder");
  assert.equal(machine.anchors.data.expression, "[CommonAppDataFolder]UCI Arena Manager");
});

test("policy rejects unknown missing and wrong-type field families", () => {
  for (const mutate of [
    (p) => { p.extra = true; },
    (p) => { delete p.platform; },
    (p) => { p.policy_version = 1.5; },
    (p) => { p.registration.extra = true; },
    (p) => { delete p.registration.values.scope; },
    (p) => { p.stable_paths.extra = "bin/x"; },
    (p) => { p.scopes["per-user"].roots.extra = { relative_to: "data", segments: ["x"] }; },
    (p) => { p.scopes["per-user"].roots.cache.segments = "cache"; },
  ]) {
    const candidate = clone();
    mutate(candidate);
    assert.throws(() => validateInstallLayoutPolicy(candidate), TypeError);
  }
});

test("policy rejects unsafe Windows segments and relative paths", () => {
  const invalidSegments = ["", ".", "..", "a/b", "a\\b", "a:", "x.", "x ", "CON", "com1.txt", "bad\u0001"];
  for (const value of invalidSegments) {
    const candidate = clone();
    candidate.scopes["per-user"].roots.cache.segments = [value];
    assert.throws(() => validateInstallLayoutPolicy(candidate), TypeError, value);
  }
  for (const value of ["/bin/x", "C:/bin/x", "bin\\x", "bin//x", "bin/../x", "NUL/x", "bin/x."]) {
    const candidate = clone();
    candidate.stable_paths.setup_launcher = value;
    assert.throws(() => validateInstallLayoutPolicy(candidate), TypeError, value);
  }
});

test("policy rejects scope root and case-insensitive identity conflicts", () => {
  const cases = [
    (p) => { p.scopes["per-user"].privilege = "machine"; },
    (p) => { p.scopes["per-user"].roots.integration.anchor = "machine_program_files"; },
    (p) => { p.scopes["per-user"].roots.install.relative_to = "data"; },
    (p) => { p.scopes["per-user"].roots.cache.segments = ["WORKSPACES"]; },
    (p) => { p.registration.values.scope = p.registration.values.schema.toLowerCase(); },
    (p) => { p.stable_paths.descriptor = p.stable_paths.scope_marker.toUpperCase(); },
    (p) => { p.stable_paths.policy = "share/uci-arena/layout-policies/wrong-v2.json"; },
  ];
  for (const mutate of cases) {
    const candidate = clone();
    mutate(candidate);
    assert.throws(() => validateInstallLayoutPolicy(candidate), TypeError);
  }
});

test("resolver rejects invalid missing and overlapping anchors", () => {
  for (const value of ["relative", "C:relative", "C:/forward", "C:\\", "C:\\x\\..\\y", "\\\\?\\C:\\x", "C:\\x."]) {
    assert.throws(() => resolveInstallLayout({
      policy,
      scope: "per-user",
      anchors: { user_local_app_data: value },
    }), TypeError, value);
  }
  assert.throws(() => resolveInstallLayout({ policy, scope: "per-user", anchors: {} }), TypeError);

  const candidate = clone();
  candidate.scopes["per-user"].roots.data.segments = ["Programs", "UCI Arena Manager", "data"];
  assert.throws(() => resolveInstallLayout({ policy: candidate, scope: "per-user", anchors }), /overlap/u);
});

test("resolver never reads environment roots", () => {
  const prior = process.env.LOCALAPPDATA;
  process.env.LOCALAPPDATA = "Z:\\secret-substitute";
  try {
    const result = resolveInstallLayout({ policy, scope: "per-user", anchors });
    assert.equal(result.integration_root.startsWith("C:\\Users\\Example"), true);
    assert.equal(JSON.stringify(result).includes("secret-substitute"), false);
  } finally {
    if (prior === undefined) delete process.env.LOCALAPPDATA;
    else process.env.LOCALAPPDATA = prior;
  }
});

test("one semantic policy change propagates through resolver and WiX projection", () => {
  const candidate = clone();
  candidate.scopes["per-user"].roots.cache.segments = ["cache-v2"];
  const resolved = resolveInstallLayout({ policy: candidate, scope: "per-user", anchors });
  const wix = buildWindowsWixLayoutProjection({ policy: candidate, scope: "per-user" });
  assert.equal(resolved.roots.cache, `${resolved.roots.data}\\cache-v2`);
  assert.equal(wix.roots.cache.expression, "cache-v2");
});

test("diagnostics are bounded and do not echo secret policy values", () => {
  const secret = `secret-${"x".repeat(600)}`;
  const candidate = clone();
  candidate.unexpected = secret;
  assert.throws(() => validateInstallLayoutPolicy(candidate), (error) => {
    assert.equal(error.message.length <= 512, true);
    assert.equal(error.message.includes(secret), false);
    return true;
  });
});
