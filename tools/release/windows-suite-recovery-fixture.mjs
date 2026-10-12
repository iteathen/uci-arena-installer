// Native, uniquely owned reproduction of a later EXE failure after an MSI major upgrade.
// This runner deliberately asserts the recovery guarantee and is RED on the old chain.
import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const xml = (value) => String(value).replaceAll("&", "&amp;").replaceAll('"', "&quot;").replaceAll("<", "&lt;");
const guid = () => `{${randomUUID().toUpperCase()}}`;
function run(executable, arguments_, expected, label) {
  const result = spawnSync(executable, arguments_, {
    encoding: "utf8", windowsHide: true, timeout: 120_000,
  });
  if (result.error || !expected.includes(result.status)) {
    throw new Error(`${label}: ${result.error?.message ?? result.status}\n${result.stdout}\n${result.stderr}`);
  }
  return result;
}

export function runManagerRecoveryFixture(evidenceParent) {
  assert.equal(process.platform, "win32", "This proof executes Windows Installer and Burn.");
  const parent = path.resolve(evidenceParent);
  assert.ok(/^[A-Za-z]:\\/u.test(parent), "An explicit absolute evidence parent is required.");
  const identity = randomUUID();
  const root = path.join(parent, `manager-msi-recovery-${identity}`);
  mkdirSync(root, { recursive: false });
  const program = path.join(root, "program");
  const registry = `Software\\UCI Arena\\Tests\\ManagerMsiRecovery\\${identity}`;
  const upgradeCode = guid();
  const oldProduct = guid();
  const newProduct = guid();
  const fileComponent = guid();
  const owner = { schema: "arena_manager_recovery_fixture_v1", identity, root, registry, upgradeCode, oldProduct, newProduct };
  writeFileSync(path.join(root, "ownership.json"), JSON.stringify(owner, null, 2) + "\n");
  const buildMsi = (name, version, productCode, bytes) => {
    const payload = path.join(root, `${name}-manager.bin`);
    const source = path.join(root, `${name}.wxs`);
    const msi = path.join(root, `${name}.msi`);
    writeFileSync(payload, bytes);
    writeFileSync(source, [
      '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
      `  <Package Name="UCI Arena isolated recovery fixture ${identity}" Manufacturer="UCI Arena tests" Version="${version}" ProductCode="${productCode}" UpgradeCode="${upgradeCode}" Scope="perUser" InstallerVersion="500" Compressed="yes">`,
      '    <MediaTemplate EmbedCab="yes" />',
      '    <MajorUpgrade Schedule="afterInstallInitialize" DowngradeErrorMessage="Newer fixture installed." />',
      `    <SetProperty Id="INSTALL_ROOT" Value="${xml(program)}" Before="CostFinalize" Sequence="both" />`,
      '    <StandardDirectory Id="LocalAppDataFolder"><Directory Id="INSTALL_ROOT" Name="IsolatedFixture">',
      `      <Component Id="FixtureManager" Guid="${fileComponent}">`,
      `        <File Id="FixtureManagerFile" Source="${xml(payload)}" Name="fixture-manager.bin" />`,
      `        <RegistryValue Root="HKCU" Key="${xml(registry)}" Name="Version" Type="string" Value="${version}" KeyPath="yes" />`,
      '      </Component>',
      '    </Directory></StandardDirectory>',
      '    <Feature Id="Main"><ComponentRef Id="FixtureManager" /></Feature>',
      '  </Package>',
      '</Wix>',
    ].join("\n"));
    run("wix", ["build", "-arch", "x64", "-o", msi, source], [0], `${name} MSI build`);
    return msi;
  };
  const oldBytes = "prior-working-manager-bytes\n";
  const newBytes = "changed-target-manager-bytes\n";
  const oldMsi = buildMsi("old", "1.0.0", oldProduct, oldBytes);
  const newMsi = buildMsi("target", "1.0.1", newProduct, newBytes);
  const failSource = path.join(root, "late-failure.cs");
  const failureExe = path.join(root, "late-failure.exe");
  writeFileSync(failSource, "public static class LateFailure { public static int Main(string[] args) { return args.Length == 1 && args[0] == \"remove\" ? 0 : 17; } }\n");
  const compiler = path.join(process.env.WINDIR ?? "C:\\Windows", "Microsoft.NET", "Framework64", "v4.0.30319", "csc.exe");
  run(compiler, ["/nologo", `/out:${failureExe}`, failSource], [0], "late-failure executable build");
  const bundleSource = path.join(root, "target-bundle.wxs");
  const bundle = path.join(root, "target-bundle.exe");
  writeFileSync(bundleSource, [
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs" xmlns:bal="http://wixtoolset.org/schemas/v4/wxs/bal">',
    `  <Bundle Name="UCI Arena isolated recovery fixture ${identity}" Manufacturer="UCI Arena tests" Version="1.0.1" UpgradeCode="${guid()}">`,
    '    <BootstrapperApplication><bal:WixStandardBootstrapperApplication Theme="hyperlinkLicense" LicenseUrl="" /></BootstrapperApplication>',
    '    <Chain>',
    `      <MsiPackage Id="ManagerCore" SourceFile="${xml(newMsi)}" Visible="no" Vital="yes" />`,
    `      <ExePackage Id="SuiteComponents" SourceFile="${xml(failureExe)}" InstallArguments="fail" UninstallArguments="remove" DetectCondition="WixBundleInstalled" Protocol="none" PerMachine="no" Permanent="no" Vital="yes" />`,
    '    </Chain>',
    '  </Bundle>',
    '</Wix>',
  ].join("\n"));
  run("wix", ["build", "-arch", "x64", "-ext", "WixToolset.BootstrapperApplications.wixext", "-o", bundle, bundleSource], [0], "fixture bundle build");
  let failure;
  try {
    run("msiexec", ["/i", oldMsi, "/qn", "/norestart", "/l*v", path.join(root, "old-install.log")], [0], "old fixture install");
    assert.equal(readFileSync(path.join(program, "fixture-manager.bin"), "utf8"), oldBytes);
    const result = spawnSync(bundle, ["/install", "/quiet", "/norestart", "/log", path.join(root, "late-failure.log")], {
      encoding: "utf8", windowsHide: true, timeout: 120_000,
    });
    assert.ifError(result.error);
    assert.notEqual(result.status, 0, "The later EXE must fail.");
    const burn = readFileSync(path.join(root, "late-failure.log"), "utf8");
    assert.match(burn, /Applied execute package: ManagerCore, result: 0x0/u);
    assert.match(burn, /SuiteComponents exited with code: 0x11/u);
    assert.doesNotMatch(burn, /Applying execute package: \{[\w-]+\}/u, "No historical bundle executor is used.");
    const observed = {
      manager_exists: existsSync(path.join(program, "fixture-manager.bin")),
      manager_bytes: existsSync(path.join(program, "fixture-manager.bin")) ? readFileSync(path.join(program, "fixture-manager.bin"), "utf8") : null,
      private_registry_result: spawnSync("reg", ["query", `HKCU\\${registry}`, "/v", "Version"], { encoding: "utf8", windowsHide: true }).stdout,
      burn_exit: result.status,
    };
    writeFileSync(path.join(root, "observed.json"), JSON.stringify(observed, null, 2) + "\n");
    assert.equal(observed.manager_bytes, oldBytes, "Late suite failure must restore the previous Manager bytes.");
    assert.match(observed.private_registry_result, /1\.0\.0/u, "Previous MSI-owned registration must be restored.");
  } catch (error) {
    failure = error;
  } finally {
    for (const [name, msi] of [["target", newMsi], ["old", oldMsi]]) {
      run("msiexec", ["/x", msi, "/qn", "/norestart", "/l*v", path.join(root, `${name}-cleanup.log`)], [0, 1605], `${name} fixture cleanup`);
    }
    run(bundle, ["/uninstall", "/quiet", "/norestart", "/log", path.join(root, "bundle-cleanup.log")], [0], "bundle fixture cleanup");
    assert.equal(existsSync(path.join(program, "fixture-manager.bin")), false, "Fixture payload must be absent after cleanup.");
    const registration = spawnSync("reg", ["query", `HKCU\\${registry}`], { encoding: "utf8", windowsHide: true });
    assert.notEqual(registration.status, 0, "Fixture registry state must be absent after cleanup.");
    writeFileSync(path.join(root, "cleanup.json"), JSON.stringify({ payload_absent: true, registry_absent: true }, null, 2) + "\n");
  }
  if (failure) {
    failure.message += `\nNative fixture evidence: ${root}`;
    throw failure;
  }
  return root;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  assert.equal(process.argv.length, 3, "Usage: node windows-suite-recovery-fixture.mjs <absolute-existing-evidence-parent>");
  console.log(runManagerRecoveryFixture(process.argv[2]));
}
