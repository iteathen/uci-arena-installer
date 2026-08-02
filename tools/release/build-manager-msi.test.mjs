import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
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
import { fileURLToPath } from "node:url";

import {
  buildManagerMsi,
  buildManagerMsiSource,
} from "./build-manager-msi.mjs";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), "../..",
);
const policyPath = path.join(
  repositoryRoot, "packaging/windows/manager-integration.json",
);
const iconPath = path.join(
  repositoryRoot, "packaging/icons/uci-arena-installer.ico",
);

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-msi-test-"));
  const managerRoot = path.join(root, "manager component");
  const installerRoot = path.join(root, "installer runtime");
  const integrationRoot = path.join(root, "installer integration");
  mkdirSync(path.join(managerRoot, "bin"), { recursive: true });
  mkdirSync(path.join(managerRoot, "share", "uci-arena", "web"), { recursive: true });
  mkdirSync(path.join(installerRoot, "bin"), { recursive: true });
  mkdirSync(integrationRoot, { recursive: true });
  const relativeFiles = [
    "bin/arena-service.exe",
    "bin/arena.exe",
    "bin/arena-broker.exe",
    "bin/uci-arena-manager.exe",
    "share/uci-arena/web/index.html",
  ];
  for (const relative of relativeFiles) {
    const filename = path.join(managerRoot, ...relative.split("/"));
    writeFileSync(filename, `fixture:${relative}\n`);
  }
  const manifest = {
    schema: "arena_install_component_v1",
    schema_version: 1,
    component_id: "manager.core",
    component_version: "0.3.8",
    target_triple: "windows-x86_64",
    entrypoints: {
      service: "bin/arena-service.exe",
      cli: "bin/arena.exe",
      broker: "bin/arena-broker.exe",
      desktop: "bin/uci-arena-manager.exe",
    },
    dependencies: [],
    capabilities: ["control_plane", "broker", "desktop_host"],
    startup: { windows: "windows_service" },
    data_paths: ["manager_state"],
    files: relativeFiles.map((relative) => {
      const filename = path.join(managerRoot, ...relative.split("/"));
      return {
        path: relative,
        sha256: sha256(filename),
        size_bytes: statSync(filename).size,
      };
    }),
  };
  writeFileSync(
    path.join(managerRoot, "arena-component.json"),
    `${JSON.stringify(manifest, null, 2)}\n`,
  );
  const setupExecutable = path.join(installerRoot, "bin", "arena-setup.exe");
  const runtimeDependency = path.join(installerRoot, "bin", "archive.dll");
  const perUserMarker = path.join(integrationRoot, "per-user.txt");
  const perMachineMarker = path.join(integrationRoot, "per-machine.txt");
  writeFileSync(setupExecutable, "MZinstaller-owned-setup");
  writeFileSync(runtimeDependency, "installer-runtime-dependency");
  const runtimeFiles = ["bin/arena-setup.exe", "bin/archive.dll"].map((relative) => {
    const filename = path.join(installerRoot, ...relative.split("/"));
    return {
      path: relative,
      sha256: sha256(filename),
      size_bytes: statSync(filename).size,
    };
  });
  writeFileSync(
    path.join(installerRoot, "arena-installer-runtime.json"),
    `${JSON.stringify({
      schema: "arena_installer_runtime_v1",
      schema_version: 1,
      product_id: "uci-arena-installer",
      runtime_version: "0.1.0",
      target_triple: "windows-x86_64",
      entrypoints: { setup: "bin/arena-setup.exe" },
      files: runtimeFiles,
    }, null, 2)}\n`,
  );
  writeFileSync(perUserMarker, "per-user\n");
  writeFileSync(perMachineMarker, "per-machine\n");
  return {
    root,
    managerRoot,
    installerRoot,
    perUserMarker,
    perMachineMarker,
  };
}

function options(f, scope) {
  return {
    managerRoot: f.managerRoot,
    installerRoot: f.installerRoot,
    installerVersion: "0.1.0",
    scopeMarker: scope === "per-user" ? f.perUserMarker : f.perMachineMarker,
    policyPath,
    version: "0.3.8",
    scope,
    iconPath,
  };
}

test("Manager MSI source applies fixed-scope integration without source-tree knowledge", () => {
  const f = fixture();
  try {
    const user = buildManagerMsiSource(options(f, "per-user"));
    const machine = buildManagerMsiSource(options(f, "per-machine"));
    assert.match(user, /Scope="perUser"/u);
    assert.match(user, /Root="HKCU"/u);
    assert.match(user, /System="no"/u);
    assert.doesNotMatch(user, /ServiceInstall/u);
    assert.match(machine, /Scope="perMachine"/u);
    assert.match(machine, /ServiceInstall/u);
    assert.match(machine, /Account="NT AUTHORITY\\LocalService"/u);
    assert.match(machine, /System="yes"/u);
    assert.match(machine, /arena-setup\.exe/u);
    assert.match(machine, /archive\.dll/u);
    assert.match(machine, /installer-runtime\.json/u);
    assert.match(machine, /install-scope\.txt/u);
    assert.doesNotMatch(machine, /CMakeLists|source checkout|\.\.\\uci-arena-manager/iu);
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Manager MSI rejects a mismatched Manager and Installer runtime collision", () => {
  const f = fixture();
  try {
    const managerShared = path.join(f.managerRoot, "bin", "shared.dll");
    writeFileSync(managerShared, "manager-version");
    const managerManifestPath = path.join(f.managerRoot, "arena-component.json");
    const managerManifest = JSON.parse(readFileSync(managerManifestPath, "utf8"));
    managerManifest.files.push({
      path: "bin/shared.dll",
      sha256: sha256(managerShared),
      size_bytes: statSync(managerShared).size,
    });
    writeFileSync(managerManifestPath, `${JSON.stringify(managerManifest, null, 2)}\n`);

    const runtimeShared = path.join(f.installerRoot, "bin", "shared.dll");
    writeFileSync(runtimeShared, "installer-version");
    const runtimeManifestPath = path.join(
      f.installerRoot, "arena-installer-runtime.json",
    );
    const runtimeManifest = JSON.parse(readFileSync(runtimeManifestPath, "utf8"));
    runtimeManifest.files.push({
      path: "bin/shared.dll",
      sha256: sha256(runtimeShared),
      size_bytes: statSync(runtimeShared).size,
    });
    writeFileSync(runtimeManifestPath, `${JSON.stringify(runtimeManifest, null, 2)}\n`);

    assert.throws(
      () => buildManagerMsiSource(options(f, "per-user")),
      /runtime path collision: bin\/shared\.dll/u,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Manager MSI source rejects component version drift before packaging", () => {
  const f = fixture();
  try {
    assert.throws(
      () => buildManagerMsiSource({ ...options(f, "per-user"), version: "0.3.9" }),
      /does not match the requested release identity/u,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("WiX compiles both generated fixed-scope Manager MSIs", {
  skip: process.platform !== "win32"
    || spawnSync("wix", ["--version"], { windowsHide: true }).status !== 0,
}, () => {
  const f = fixture();
  try {
    for (const scope of ["per-user", "per-machine"]) {
      const output = path.join(f.root, `${scope}.msi`);
      buildManagerMsi({ ...options(f, scope), output });
      assert.ok(statSync(output).size > 0);
    }
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});
