import assert from "node:assert/strict";
import {
  mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";

import { assembleSuiteUpdatePackage } from "./build-suite-update-package.mjs";
import { sha256File } from "./release-lock.mjs";

function tarEntries(filename) {
  const archive = gunzipSync(readFileSync(filename));
  const entries = new Map();
  let offset = 0;
  while (offset + 512 <= archive.length) {
    const header = archive.subarray(offset, offset + 512);
    if (header.every((value) => value === 0)) break;
    const name = header.subarray(0, 100).toString("utf8").replace(/\0.*$/u, "");
    const sizeText = header.subarray(124, 136).toString("ascii").replace(/\0.*$/u, "").trim();
    const size = Number.parseInt(sizeText || "0", 8);
    entries.set(name, archive.subarray(offset + 512, offset + 512 + size));
    offset += 512 + Math.ceil(size / 512) * 512;
  }
  return entries;
}

function extractTar(filename, destination) {
  for (const [name, contents] of tarEntries(filename)) {
    assert.match(name, /^[A-Za-z0-9_.-]+(?:\/[A-Za-z0-9_.-]+)*$/u);
    const output = path.join(destination, ...name.split("/"));
    mkdirSync(path.dirname(output), { recursive: true });
    writeFileSync(output, contents);
  }
}

function fixture(root) {
  const definitions = [
    { role: "manager", id: "manager.core", requires: [] },
    { role: "runtime", id: "node_runtime.private", requires: [] },
    { role: "provider", id: "lichess.adapter", requires: ["node_runtime.private"] },
  ];
  const composition = {
    schema: "arena_product_composition_v1",
    product_release: "1.2.3",
    installer: {
      repository: "iteathen/uci-arena-installer",
      minimum_version: "0.2.0",
    },
    components: definitions.map((entry) => ({
      role: entry.role,
      component_id: entry.id,
      version: "1.0.0",
      repository: entry.id === "manager.core"
        ? "iteathen/uci-arena-manager" : "iteathen/provider-fixture",
      required: entry.id !== "lichess.adapter",
      default_enabled: true,
      requires: entry.requires,
    })),
  };
  const candidates = definitions.map((entry, index) => {
    const artifactPath = path.join(root, `${entry.id}.tar.gz`);
    writeFileSync(artifactPath, `artifact-${entry.id}`);
    return {
      repository: composition.components[index].repository,
      artifactPath,
      artifactSizeBytes: readFileSync(artifactPath).length,
      manifestSizeBytes: 128,
      providerResult: { artifact_type: "archive" },
      lockRow: {
        component_id: entry.id,
        component_version: "1.0.0",
        target_triple: "windows-x86_64",
        artifact_name: path.basename(artifactPath),
        artifact_sha256: sha256File(artifactPath),
      },
      manifest: {
        schema: "arena_install_component_v1",
        schema_version: 1,
        component_id: entry.id,
        component_version: "1.0.0",
        dependencies: entry.requires,
        files: [],
      },
    };
  });
  return { composition, candidates };
}

test("routine suite update is deterministic and has no native build path", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-update-"));
  try {
    const { composition, candidates } = fixture(root);
    const applyScript = path.join(root, "apply.ps1");
    writeFileSync(applyScript, "Write-Output 'apply'\n");
    const common = {
      composition,
      candidates,
      selectedComponents: ["lichess.adapter"],
      sourceCommit: "a".repeat(40),
      sourceEpoch: 1_700_000_000,
      compositionSha256: "b".repeat(64),
      releaseLockSha256: "c".repeat(64),
      inputAuthority: "diagnostic_review_lock",
      applyScript,
    };
    const first = assembleSuiteUpdatePackage({
      ...common,
      outputRoot: path.join(root, "first"),
    });
    const second = assembleSuiteUpdatePackage({
      ...common,
      outputRoot: path.join(root, "second"),
    });
    assert.equal(first.build_path, "package_only_no_native_compilation");
    assert.equal(first.full_installer_build_required, false);
    assert.deepEqual(first.selected_components, ["lichess.adapter"]);
    assert.deepEqual(first.dependency_closure, ["lichess.adapter", "node_runtime.private"]);
    assert.equal(first.artifact.sha256, second.artifact.sha256);
    const entries = tarEntries(path.join(root, "first", first.artifact.name));
    assert.ok(entries.has("release/payloads/lichess.adapter.tar.gz"));
    assert.ok(entries.has("release/payloads/node_runtime.private.tar.gz"));
    assert.equal(entries.has("release/payloads/manager.core.tar.gz"), false);
    const catalog = JSON.parse(entries.get("release/catalog.json").toString("utf8"));
    assert.deepEqual(
      catalog.components.map((component) => component.id).sort(),
      ["lichess.adapter", "manager.core", "node_runtime.private"],
    );
    assert.deepEqual(catalog.presets.manager_only, ["manager.core"]);
    assert.deepEqual(
      catalog.components.filter(
        (component) => first.dependency_closure.includes(component.id),
      ).flatMap(
        (component) => Object.values(component.artifacts).map((artifact) => artifact.source),
      ).sort(),
      [...entries.keys()]
        .filter((name) => name.startsWith("release/payloads/"))
        .map((name) => name.replace(/^release\//u, ""))
        .sort(),
    );
    assert.equal(
      readFileSync(path.join(root, "first", "suite-update-result.json"), "utf8")
        .includes("cmake"),
      false,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("manager changes stay on the full installer path", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-update-manager-"));
  try {
    const { composition, candidates } = fixture(root);
    const applyScript = path.join(root, "apply.ps1");
    writeFileSync(applyScript, "Write-Output 'apply'\n");
    assert.throws(() => assembleSuiteUpdatePackage({
      composition,
      candidates,
      selectedComponents: ["manager.core"],
      outputRoot: path.join(root, "output"),
      sourceCommit: "a".repeat(40),
      sourceEpoch: 1_700_000_000,
      compositionSha256: "b".repeat(64),
      releaseLockSha256: "c".repeat(64),
      inputAuthority: "diagnostic_review_lock",
      applyScript,
    }), /full MSI\/installer build path/u);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("apply script reconciles exact retained payloads before arena-setup", {
  skip: process.platform !== "win32",
}, () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-update-media-"));
  try {
    const { composition, candidates } = fixture(root);
    const applyScript = fileURLToPath(
      new URL("../../packaging/windows/apply-suite-update.ps1", import.meta.url),
    );
    const result = assembleSuiteUpdatePackage({
      composition,
      candidates,
      selectedComponents: ["lichess.adapter"],
      outputRoot: path.join(root, "output"),
      sourceCommit: "a".repeat(40),
      sourceEpoch: 1_700_000_000,
      compositionSha256: "b".repeat(64),
      releaseLockSha256: "c".repeat(64),
      inputAuthority: "diagnostic_review_lock",
      applyScript,
    });
    const packageRoot = path.join(root, "package");
    extractTar(path.join(root, "output", result.artifact.name), packageRoot);
    const dataRoot = path.join(root, "data");
    const retainedManager = path.join(
      dataRoot, "release", "payloads", "manager.core.tar.gz",
    );
    mkdirSync(path.dirname(retainedManager), { recursive: true });
    writeFileSync(retainedManager, readFileSync(path.join(root, "manager.core.tar.gz")));
    const programRoot = path.join(root, "program");
    mkdirSync(path.join(programRoot, "bin"), { recursive: true });
    const setup = path.join(programRoot, "bin", "arena-setup.exe");
    const layoutCall = path.join(root, "layout-called.txt");
    const projection = {
      schema: "arena_package_layout_selection_v1",
      scope: "per-user",
      integration_root: programRoot,
      roots: {
        data: dataRoot,
        install: path.join(programRoot, "installed"),
        cache: path.join(dataRoot, "cache"),
        workspace: path.join(dataRoot, "workspaces"),
        knowledge: path.join(dataRoot, "knowledge"),
      },
    };
    const encoded = (value) => Buffer.from(value, "utf8").toString("base64");
    const source = path.join(root, "FixtureSetup.cs");
    writeFileSync(source, `using System; using System.IO; using System.Text;
public static class FixtureSetup {
  public static int Main(string[] args) {
    if (args.Length != 4 || args[0] != "--install-scope" || args[1] != "per-user" ||
        args[2] != "--json" || args[3] != "layout") return 2;
    File.WriteAllText(Encoding.UTF8.GetString(Convert.FromBase64String("${encoded(layoutCall)}")), String.Join("\\n", args));
    Console.WriteLine(Encoding.UTF8.GetString(Convert.FromBase64String("${encoded(JSON.stringify(projection))}")));
    return 0;
  }
}`);
    const compileScript = path.join(root, "compile-fixture.ps1");
    const psLiteral = (value) => `'${value.replaceAll("'", "''")}'`;
    writeFileSync(compileScript,
      `$ErrorActionPreference = 'Stop'\nAdd-Type -Path ${psLiteral(source)} -OutputAssembly ${psLiteral(setup)} -OutputType ConsoleApplication\n`);
    const compiled = spawnSync("powershell.exe", ["-NoProfile", "-File", compileScript], {
      encoding: "utf8", timeout: 30_000,
    });
    assert.equal(compiled.status, 0, compiled.stderr);
    const runtimeRoot = path.join(programRoot, "libexec", "uci-arena-installer");
    mkdirSync(runtimeRoot, { recursive: true });
    writeFileSync(path.join(runtimeRoot, "arena-installer-runtime.json"), JSON.stringify({
      schema: "arena_installer_runtime_v1",
      runtime_version: "0.2.0",
    }));
    const arguments_ = [
      "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
      path.join(packageRoot, "apply-update.ps1"),
      "-Scope", "per-user",
      "-ManagerProgramRoot", programRoot,
      "-DataRoot", dataRoot,
      "-ValidateMediaOnly",
    ];
    const validation = spawnSync("powershell.exe", arguments_, {
      encoding: "utf8",
      timeout: 30_000,
    });
    assert.equal(validation.status, 0, validation.stderr);
    assert.equal(readFileSync(layoutCall, "utf8"), "--install-scope\nper-user\n--json\nlayout");
    const summary = JSON.parse(validation.stdout.trim().split(/\r?\n/u).at(-1));
    assert.deepEqual(summary, {
      schema: "arena_suite_update_media_validation_v1",
      state: "validated",
      suite_version: "1.2.3",
      catalog_components: 3,
      packaged_payloads: 2,
      retained_payloads: 1,
    });
    assert.deepEqual(
      readdirSync(dataRoot).filter((name) => name.startsWith("routine-update-media-")),
      [],
    );

    rmSync(retainedManager);
    const missing = spawnSync("powershell.exe", arguments_, {
      encoding: "utf8",
      timeout: 30_000,
    });
    assert.notEqual(missing.status, 0);
    assert.match(
      `${missing.stdout}\n${missing.stderr}`,
      /unavailable from its exact retained release channel authority/u,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
