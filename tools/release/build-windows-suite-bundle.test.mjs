import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  existsSync,
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
  buildWindowsSuiteBundle,
  buildWindowsSuiteBundleSource,
} from "./build-windows-suite-bundle.mjs";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), "../..",
);
const policyPath = path.join(repositoryRoot, "packaging/windows/suite-bundle.json");
const iconPath = path.join(repositoryRoot, "packaging/icons/uci-arena-installer.ico");
const localizationPath = path.join(
  repositoryRoot, "packaging/windows/suite-theme.en-us.wxl",
);
const wixAvailable = process.platform === "win32"
  && spawnSync("wix", ["--version"], { windowsHide: true }).status === 0;
const builtBAFunctionsPath = path.join(
  repositoryRoot, "build/windows-release/Release/arena-bundle-bafunctions.dll",
);
const upgradeCycleEnabled = wixAvailable
  && process.env.ARENA_RUN_WINDOWS_BUNDLE_UPGRADE_TEST === "1"
  && existsSync(builtBAFunctionsPath);

const features = [
  "uci_arena.vector",
  "book_forge.service",
  "syzygy.root-provider",
  "uci_arena_evidence.service",
  "lichess.adapter",
];

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-suite-bundle-test-"));
  const mediaRoot = path.join(root, "suite media");
  mkdirSync(path.join(mediaRoot, "bin"), { recursive: true });
  mkdirSync(path.join(mediaRoot, "release", "payloads"), { recursive: true });
  const setup = path.join(mediaRoot, "bin", "arena-setup.exe");
  const windowsExecutable = path.join(process.env.WINDIR ?? "C:\\Windows", "System32", "where.exe");
  if (process.platform === "win32") copyFileSync(windowsExecutable, setup);
  else writeFileSync(setup, "MZfixture");
  writeFileSync(path.join(mediaRoot, "release", "payloads", "component.tar.gz"), "payload");
  const catalog = {
    schema: "arena_install_catalog_v1",
    schema_version: 1,
    suite_version: "0.3.8",
    presets: { base: ["manager.core"] },
    feature_selection: {
      base_preset: "base",
      components: features,
      default_selected: features,
    },
    components: ["manager.core", ...features].map((id) => ({
      id,
      version: "1.0.0",
      artifacts: {},
    })),
  };
  writeFileSync(
    path.join(mediaRoot, "release", "catalog.json"),
    `${JSON.stringify(catalog, null, 2)}\n`,
  );
  return { root, mediaRoot };
}

function options(f, scope, managerMsi = path.join(f.root, "manager.msi")) {
  return {
    mediaRoot: f.mediaRoot,
    managerMsi,
    baFunctionsPath: process.platform === "win32"
      ? path.join(process.env.WINDIR ?? "C:\\Windows", "System32", "where.exe")
      : path.join(f.mediaRoot, "bin", "arena-setup.exe"),
    policyPath,
    version: "0.3.8",
    scope,
    iconPath,
    localizationPath,
    themePath: localizationPath,
    development: true,
    offline: false,
  };
}

function buildFixtureMsi(root, scope) {
  const payload = path.join(process.env.WINDIR, "System32", "where.exe");
  const machine = scope === "per-machine";
  const source = path.join(root, `${scope}.wxs`);
  const output = path.join(root, `${scope}.msi`);
  const component = machine
    ? `<Component Id="Fixture" Guid="A2550704-45A8-5529-9D1B-54C950AB5D1A"><File Source="${payload}" KeyPath="yes" /></Component>`
    : `<Component Id="Fixture" Guid="26DE4C72-D29B-55E9-BCDE-58BCBC155C0E"><File Source="${payload}" /><RegistryValue Root="HKCU" Key="Software\\UCI Arena\\Tests" Name="Fixture" Type="integer" Value="1" KeyPath="yes" /></Component>`;
  writeFileSync(source, [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
    `  <Package Name="Fixture" Version="0.3.8" Manufacturer="Fixture" UpgradeCode="${machine ? "E753DAAF-70C0-50B1-9623-10DCA0F8AB46" : "68E522D1-1765-5B40-98CC-1F2B3F2BE783"}" Scope="${machine ? "perMachine" : "perUser"}">`,
    '    <MediaTemplate EmbedCab="yes" />',
    `    <StandardDirectory Id="${machine ? "ProgramFiles64Folder" : "LocalAppDataFolder"}"><Directory Id="INSTALLROOT" Name="ArenaFixture">${component}</Directory></StandardDirectory>`,
    '    <Feature Id="Main"><ComponentRef Id="Fixture" /></Feature>',
    '  </Package>',
    '</Wix>',
  ].join("\n"));
  const result = spawnSync("wix", ["build", "-arch", "x64", "-o", output, source], {
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.status !== 0) throw new Error(result.stderr || result.stdout);
  return output;
}

function runFixture(executable, args, label) {
  const result = spawnSync(executable, args, {
    encoding: "utf8",
    windowsHide: true,
    timeout: 120_000,
  });
  if (result.error || result.status !== 0) {
    throw new Error([
      `${label} failed with ${result.status ?? result.error?.message}`,
      result.stderr,
      result.stdout,
    ].filter(Boolean).join("\n"));
  }
}

function buildUpgradeFixture(root, version) {
  const suffix = version.replaceAll(".", "-");
  const msiSource = path.join(root, `fixture-${suffix}.wxs`);
  const msi = path.join(root, `fixture-${suffix}.msi`);
  writeFileSync(msiSource, [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
    `  <Package Name="Arena Bundle Upgrade Fixture" Version="${version}" Manufacturer="UCI Arena tests" UpgradeCode="7FF52046-9320-46D0-8798-AE1B1C1CAEA8" Scope="perUser">`,
    '    <MediaTemplate EmbedCab="yes" />',
    '    <StandardDirectory Id="LocalAppDataFolder">',
    '      <Directory Id="FIXTUREROOT" Name="UCI Arena Bundle Upgrade Fixture">',
    '        <Component Id="Fixture" Guid="79B13CF1-A1E2-4439-B3DF-22E4BA18ED31">',
    `          <RegistryValue Root="HKCU" Key="Software\\UCI Arena\\Tests\\BundleUpgradeGuard" Name="Version" Type="string" Value="${version}" KeyPath="yes" />`,
    '        </Component>',
    '      </Directory>',
    '    </StandardDirectory>',
    '    <Feature Id="Main"><ComponentRef Id="Fixture" /></Feature>',
    '  </Package>',
    '</Wix>',
  ].join("\n"));
  runFixture("wix", ["build", "-arch", "x64", "-o", msi, msiSource], "fixture MSI build");

  const bundleSource = path.join(root, `bundle-${suffix}.wxs`);
  const bundle = path.join(root, `bundle-${suffix}.exe`);
  writeFileSync(bundleSource, [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs" xmlns:bal="http://wixtoolset.org/schemas/v4/wxs/bal">',
    `  <Bundle Name="Arena Bundle Upgrade Fixture" Manufacturer="UCI Arena tests" Version="${version}" UpgradeCode="85A237D1-EE4F-4299-8A77-14F4AA5CA0D5">`,
    '    <BootstrapperApplication>',
    '      <bal:WixStandardBootstrapperApplication Theme="hyperlinkLicense" LicenseUrl="" />',
    `      <Payload Id="ArenaBundleBAFunctions" SourceFile="${builtBAFunctionsPath}" bal:BAFunctions="yes" />`,
    '    </BootstrapperApplication>',
    '    <Chain>',
    `      <MsiPackage Id="Fixture" SourceFile="${msi}" Visible="no" Vital="yes" />`,
    '    </Chain>',
    '  </Bundle>',
    '</Wix>',
  ].join("\n"));
  runFixture("wix", [
    "build", "-arch", "x64",
    "-ext", "WixToolset.BootstrapperApplications.wixext",
    "-o", bundle, bundleSource,
  ], "fixture bundle build");
  return bundle;
}

test("suite bundle derives default-on Modify features and lifecycle commands from contracts", () => {
  const f = fixture();
  try {
    writeFileSync(path.join(f.root, "manager.msi"), "fixture-msi");
    const { source, theme } = buildWindowsSuiteBundleSource(options(f, "per-user"));
    assert.match(source, /bal:BAFunctions="yes"/u);
    assert.match(source, /ArenaBundleBAFunctions/u);
    assert.throws(
      () => buildWindowsSuiteBundleSource({
        ...options(f, "per-user"), baFunctionsPath: undefined,
      }),
      /requires a bootstrapper lifecycle adapter/u,
    );
    for (const component of features) {
      assert.match(source, new RegExp(`--bundle-feature ${component.replaceAll(".", "\\.")}=\\[Install`, "u"));
    }
    assert.match(source, /upgrade --yes/u);
    assert.match(source, /repair --yes/u);
    assert.match(source, /uninstall --all --yes/u);
    assert.doesNotMatch(source, /purge/u);
    assert.match(source, /PerMachine="no"/u);
    assert.doesNotMatch(source, /restart-manager-service/u);
    assert.match(theme, /Vector GPU engine/u);
    assert.match(theme, /Root tablebase provider \(optional local data\)/u);
    assert.match(theme, /installed disabled until account setup/u);
    assert.match(theme, /Everything is selected by default/u);

    const offline = buildWindowsSuiteBundleSource({
      ...options(f, "per-user"),
      offline: true,
    });
    assert.equal(offline.source.match(/--offline/gu)?.length, 2);
    assert.match(offline.source, /InstallArguments="[^"]*upgrade --yes --offline /u);
    assert.match(offline.source, /RepairArguments="[^"]*repair --yes --offline /u);
    assert.doesNotMatch(
      offline.source,
      /(?:InstallArguments|RepairArguments)="[^"]*--offline[^"]*(?:upgrade|repair) /u,
    );
    assert.match(offline.source, /Name="UCI Arena Suite Setup \(Personal\) Offline"/u);
    assert.doesNotMatch(source, /--offline/u);
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("suite bundle rejects secret-bearing media before WiX", () => {
  const f = fixture();
  try {
    writeFileSync(path.join(f.root, "manager.msi"), "fixture-msi");
    mkdirSync(path.join(f.mediaRoot, "secrets"));
    writeFileSync(path.join(f.mediaRoot, "secrets", "credential.txt"), "opaque");
    assert.throws(
      () => buildWindowsSuiteBundleSource(options(f, "per-user")),
      /never contain credentials/u,
    );
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("WiX compiles Installer-owned Personal and All users suite bundles", {
  skip: !wixAvailable,
}, () => {
  const f = fixture();
  try {
    for (const scope of ["per-user", "per-machine"]) {
      const managerMsi = buildFixtureMsi(f.root, scope);
      const output = path.join(f.root, `${scope}-suite.exe`);
      buildWindowsSuiteBundle({ ...options(f, scope, managerMsi), output });
      assert.ok(statSync(output).size > 0);
    }
  } finally {
    rmSync(f.root, { recursive: true, force: true });
  }
});

test("Personal upgrade executes one removal plan per exact related bundle", {
  skip: !upgradeCycleEnabled,
  timeout: 180_000,
}, () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-bundle-upgrade-cycle-"));
  const firstLog = path.join(root, "first.log");
  const upgradeLog = path.join(root, "upgrade.log");
  const cleanupLog = path.join(root, "cleanup.log");
  let firstBundle;
  let secondBundle;
  try {
    firstBundle = buildUpgradeFixture(root, "1.0.0");
    secondBundle = buildUpgradeFixture(root, "1.0.1");
    runFixture(firstBundle, ["-quiet", "-norestart", "-log", firstLog], "fixture install");
    runFixture(secondBundle, ["-quiet", "-norestart", "-log", upgradeLog], "fixture upgrade");

    const log = readFileSync(upgradeLog, "utf8");
    const removalPlans = log.match(/i207: Planned related bundle: .*execute: Uninstall/gu) ?? [];
    assert.equal(removalPlans.length, 1, "one exact prior bundle may receive removal authority");
    assert.doesNotMatch(log, /CreateProcessW failed|0x80070003/iu);
  } finally {
    if (secondBundle && existsSync(secondBundle)) {
      spawnSync(secondBundle, ["-uninstall", "-quiet", "-norestart", "-log", cleanupLog], {
        windowsHide: true,
        timeout: 120_000,
      });
    }
    if (firstBundle && existsSync(firstBundle)) {
      spawnSync(firstBundle, ["-uninstall", "-quiet", "-norestart"], {
        windowsHide: true,
        timeout: 120_000,
      });
    }
    spawnSync("reg.exe", [
      "delete", "HKCU\\Software\\UCI Arena\\Tests\\BundleUpgradeGuard", "/f",
    ], { windowsHide: true });
    rmSync(root, { recursive: true, force: true });
  }
});
