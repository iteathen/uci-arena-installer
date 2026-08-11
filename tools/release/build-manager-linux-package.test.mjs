import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { gunzipSync } from "node:zlib";

import {
  buildManagerLinuxPackage,
  renderDebianControl,
  renderRpmSpec,
} from "./build-manager-linux-package.mjs";
import {
  createManagerLinuxPackagePlan,
  stageManagerLinuxPackage,
} from "./manager-linux-payload.mjs";

const repositoryRoot = path.resolve(import.meta.dirname, "../..");
const policyPath = path.join(
  repositoryRoot, "packaging", "linux", "manager-integration.json",
);

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-linux-package-"));
  const managerRoot = path.join(root, "Manager component ü");
  const files = new Map([
    ["bin/arena-service", "service"],
    ["bin/arena", "cli"],
    ["bin/arena-broker", "broker"],
    ["bin/uci-arena-manager", "desktop"],
    ["share/uci-arena/web/index.html", "<html></html>"],
    ["share/uci-arena/config/broker-policy.example.json", "{}\n"],
    ["share/doc/uci-arena-manager/arena.1", ".TH arena 1\n"],
    ["share/licenses/uci-arena-manager/LICENSE", "license\n"],
    ["share/icons/uci-arena-manager/uci-arena-manager.svg", "<svg/>\n"],
    ["share/icons/uci-arena-manager/uci-arena-manager.png", "png\n"],
    ["sbom.spdx.json", "{}\n"],
    ["component-test-receipt.json", "{}\n"],
  ]);
  for (const [relative, bytes] of files) {
    const filename = path.join(managerRoot, ...relative.split("/"));
    mkdirSync(path.dirname(filename), { recursive: true });
    writeFileSync(filename, bytes);
  }
  const manifestFiles = [...files.keys()].map((relative) => {
    const filename = path.join(managerRoot, ...relative.split("/"));
    return {
      path: relative,
      size_bytes: statSync(filename).size,
      sha256: sha256(filename),
    };
  });
  writeFileSync(path.join(managerRoot, "arena-component.json"), `${JSON.stringify({
    schema: "arena_install_component_v1",
    schema_version: 1,
    component_id: "manager.core",
    component_version: "0.3.8",
    target_triple: "linux-x86_64",
    entrypoints: {
      service: "bin/arena-service",
      cli: "bin/arena",
      broker: "bin/arena-broker",
      desktop: "bin/uci-arena-manager",
    },
    files: manifestFiles,
  }, null, 2)}\n`);
  return { root, managerRoot, output: path.join(root, "stage with spaces") };
}

test("Linux package staging composes immutable Manager bytes through policy", () => {
  const f = fixture();
  try {
    const staged = stageManagerLinuxPackage({
      managerRoot: f.managerRoot,
      policyPath,
      managerVersion: "0.3.8",
      version: "0.3.9",
      outputRoot: f.output,
      sourceDateEpoch: 1_700_000_000,
    });
    assert.ok(staged.files.includes("usr/bin/arena-service"));
    assert.ok(staged.files.includes("lib/systemd/system/uci-arena-manager.service"));
    assert.ok(staged.files.includes("usr/share/applications/uci-arena-manager.desktop"));
    assert.ok(staged.files.includes("usr/share/man/man1/arena.1.gz"));
    assert.ok(staged.files.includes(
      "usr/share/uci-arena/components/manager.core/arena-component.json",
    ));
    assert.equal(
      sha256(path.join(f.managerRoot, "bin", "arena-service")),
      sha256(path.join(f.output, "usr", "bin", "arena-service")),
    );
    assert.match(
      gunzipSync(readFileSync(path.join(
        f.output, "usr", "share", "doc", "uci-arena-manager", "changelog.gz",
      ))).toString("utf8"),
      /0\.3\.9/u,
    );
    const control = renderDebianControl(staged);
    assert.match(control, /^Package: uci-arena-manager$/mu);
    assert.match(control, /^Depends: .*systemd.*webkit2gtk/mu);
    const rpm = renderRpmSpec(staged, f.output);
    assert.match(rpm, /^Name: uci-arena-manager$/mu);
    assert.match(rpm, /^%post$/mu);
    assert.match(rpm, /preserve|preserved/iu);
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Linux package policy fails closed on drift and overlapping roots", () => {
  const f = fixture();
  try {
    writeFileSync(path.join(f.managerRoot, "unexpected"), "stale");
    assert.throws(
      () => createManagerLinuxPackagePlan({
        managerRoot: f.managerRoot,
        policyPath,
        managerVersion: "0.3.8",
        version: "0.3.8",
      }),
      /inventory mismatch/u,
    );
    rmSync(path.join(f.managerRoot, "unexpected"));
    assert.throws(
      () => stageManagerLinuxPackage({
        managerRoot: f.managerRoot,
        policyPath,
        managerVersion: "0.3.8",
        version: "0.3.8",
        outputRoot: path.join(f.managerRoot, "nested"),
        sourceDateEpoch: 1_700_000_000,
      }),
      /must not overlap/u,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Linux tar adapter is deterministic and uninstall scripts preserve state", async () => {
  const f = fixture();
  try {
    const first = path.join(f.root, "first.tar.gz");
    const second = path.join(f.root, "second.tar.gz");
    const common = {
      managerRoot: f.managerRoot,
      policyPath,
      managerVersion: "0.3.8",
      version: "0.3.9",
      format: "tar",
      sourceDateEpoch: 1_700_000_000,
    };
    buildManagerLinuxPackage({ ...common, output: first });
    await new Promise((resolve) => setTimeout(resolve, 20));
    buildManagerLinuxPackage({ ...common, output: second });
    assert.equal(sha256(first), sha256(second));
    for (const relative of [
      "packaging/linux/debian/postrm",
      "packaging/linux/rpm/postrm",
    ]) {
      const body = readFileSync(path.join(repositoryRoot, relative), "utf8");
      assert.match(body, /preserve/iu);
      assert.doesNotMatch(body, /rm\s+-rf\s+.*(?:var\/lib|uci-arena)/iu);
    }
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});
