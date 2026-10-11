#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import {
  lstatSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { buildWindowsWixLayoutProjection } from "./install-layout.mjs";

const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/u;
const SCOPES = new Set(["per-user", "per-machine"]);

function parse(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    const name = flag.slice(2);
    if (Object.hasOwn(result, name)) throw new Error(`duplicate --${name}`);
    result[name] = value;
  }
  return result;
}

function xml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&apos;");
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? path.join(relative, name) : name;
    const absolute = path.join(root, child);
    const status = lstatSync(absolute);
    if (status.isSymbolicLink()) {
      throw new Error(`suite media contains a symbolic link: ${child}`);
    }
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child);
    else throw new Error(`suite media contains an unsupported entry: ${child}`);
  }
  return rows;
}

function payloadName(relative) {
  const normalized = path.normalize(relative);
  const segments = normalized.split(path.sep);
  if (segments.length === 2 && segments[0].toLowerCase() === "bin") {
    return segments[1];
  }
  return normalized;
}

function validateInputs({
  mediaRoot, managerMsi, baFunctionsPath, policyPath, version, scope, development,
}) {
  if (!VERSION_PATTERN.test(version ?? "") || !SCOPES.has(scope)) {
    throw new Error("suite bundle requires a three-field version and fixed scope");
  }
  if (typeof baFunctionsPath !== "string" || baFunctionsPath.length === 0) {
    throw new Error("suite bundle requires a bootstrapper lifecycle adapter");
  }
  const media = path.resolve(mediaRoot);
  const msi = path.resolve(managerMsi);
  const baFunctions = path.resolve(baFunctionsPath);
  if (!lstatSync(media).isDirectory() || !lstatSync(msi).isFile()
      || statSync(msi).size === 0 || !lstatSync(baFunctions).isFile()
      || statSync(baFunctions).size === 0) {
    throw new Error("suite media, Manager MSI, or bootstrapper lifecycle adapter is unavailable");
  }
  const setup = path.join(media, "bin", "arena-setup.exe");
  const catalogPath = path.join(media, "release", "catalog.json");
  if (!lstatSync(setup).isFile() || !lstatSync(catalogPath).isFile()) {
    throw new Error("suite media must contain bin/arena-setup.exe and release/catalog.json");
  }
  const entries = files(media);
  if (entries.some((relative) =>
    /(^|[\\/])secrets?([\\/]|$)|lichess[_-]?bot[_-]?token/iu.test(relative))) {
    throw new Error("suite media must never contain credentials or secret paths");
  }
  if (!development && !entries.some((relative) =>
    path.normalize(relative) === path.normalize("release/tuf/root.json"))) {
    throw new Error("official suite media requires release/tuf/root.json");
  }
  const policy = readJson(path.resolve(policyPath), "Windows suite bundle policy");
  const selectedScope = policy?.scopes?.[scope];
  if (policy?.schema !== "arena_windows_suite_bundle_v1"
      || !Array.isArray(policy.features) || policy.features.length === 0
      || !selectedScope) {
    throw new Error("invalid Windows suite bundle policy");
  }
  const catalog = readJson(catalogPath, "installation catalog");
  if (catalog?.schema !== "arena_install_catalog_v1"
      || catalog.schema_version !== 1
      || catalog.suite_version !== version
      || !Array.isArray(catalog.components)
      || !catalog.feature_selection) {
    throw new Error("installation catalog does not match the requested suite release");
  }
  const catalogFeatures = catalog.feature_selection.components;
  const defaults = new Set(catalog.feature_selection.default_selected);
  if (!Array.isArray(catalogFeatures) || !Array.isArray(catalog.feature_selection.default_selected)
      || catalogFeatures.length !== policy.features.length
      || policy.features.some((feature, index) =>
        feature.component_id !== catalogFeatures[index]
        || typeof feature.variable !== "string"
        || !/^Install[A-Za-z0-9]+$/u.test(feature.variable)
        || typeof feature.label !== "string" || feature.label.length === 0)
      || catalogFeatures.some((id) =>
        !catalog.components.some((component) => component.id === id))) {
    throw new Error("bundle feature policy does not match the catalog feature-selection contract");
  }
  return {
    media, msi, baFunctions, setup, catalogPath, entries, policy, selectedScope, defaults,
  };
}

export function buildSuiteTheme(features) {
  const boxes = features.map((feature, index) =>
    `            <Checkbox Name="${xml(feature.variable)}" X="18" Y="${140 + index * 24}" Width="-18" Height="20" TabStop="yes" FontId="3">${xml(feature.label)}</Checkbox>`);
  return [
    '<?xml version="1.0" encoding="utf-8"?>',
    '<Theme xmlns="http://wixtoolset.org/schemas/v4/thmutil">',
    '  <Font Id="0" Height="-12" Weight="500" Foreground="windowtext" Background="window">Segoe UI</Font>',
    '  <Font Id="1" Height="-24" Weight="500" Foreground="windowtext">Segoe UI</Font>',
    '  <Font Id="2" Height="-22" Weight="500" Foreground="graytext">Segoe UI</Font>',
    '  <Font Id="3" Height="-12" Weight="500" Foreground="windowtext" Background="window">Segoe UI</Font>',
    '  <Window Width="540" Height="330" HexStyle="100a0000" FontId="0" Caption="#(loc.Caption)">',
    '    <Label X="18" Y="16" Width="-18" Height="48" FontId="1" Visible="yes" DisablePrefix="yes">#(loc.Title)</Label>',
    '    <Page Name="Help">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes">#(loc.HelpHeader)</Label>',
    '      <Label X="18" Y="112" Width="-18" Height="-35" FontId="3" DisablePrefix="yes">#(loc.HelpText)</Label>',
    '      <Button Name="HelpCloseButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.HelpCloseButton)</Text><CloseWindowAction /></Button>',
    '    </Page>',
    '    <Page Name="Loading"><Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes" Visible="no" Name="CheckingForUpdatesLabel" /></Page>',
    '    <Page Name="Install">',
    '      <Hypertext Name="EulaHyperlink" X="18" Y="121" Width="-18" Height="51" TabStop="yes" FontId="3" HideWhenDisabled="yes">#(loc.InstallLicenseLinkText)</Hypertext>',
    '      <Checkbox Name="EulaAcceptCheckbox" X="-18" Y="-44" Width="300" Height="17" TabStop="yes" FontId="3" HideWhenDisabled="yes">#(loc.InstallAcceptCheckbox)</Checkbox>',
    '      <Button Name="InstallUpdateButton" X="18" Y="-14" Width="210" Height="24" TabStop="yes" FontId="0" EnableCondition="WixStdBAUpdateAvailable" HideWhenDisabled="yes">#(loc.UpdateButton)</Button>',
    '      <Button Name="OptionsButton" X="-188" Y="-14" Width="86" Height="24" TabStop="yes" FontId="0" VisibleCondition="NOT WixStdBASuppressOptionsUI"><Text>#(loc.InstallOptionsButton)</Text><ChangePageAction Page="Options" /></Button>',
    '      <Button Name="InstallButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0" ElevationShieldCondition="WixBundleAuthoredScope = 1 AND NOT WixBundleElevated">#(loc.InstallInstallButton)</Button>',
    '      <Button Name="InstallCancelButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.InstallCancelButton)</Text><CloseWindowAction /></Button>',
    '    </Page>',
    '    <Page Name="Options">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes">Choose suite components</Label>',
    '      <Label X="18" Y="112" Width="-18" Height="24" FontId="3" DisablePrefix="yes">Everything is selected by default and can be changed later through Modify.</Label>',
    ...boxes,
    '      <Button Name="OptionsOkButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.OptionsOkButton)</Text><ChangePageAction Page="Modify" Condition="WixBundleInstalled" /><ChangePageAction Page="Install" Condition="NOT WixBundleInstalled" /></Button>',
    '      <Button Name="OptionsCancelButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.OptionsCancelButton)</Text><ChangePageAction Page="Modify" Condition="WixBundleInstalled" Cancel="yes" /><ChangePageAction Page="Install" Condition="NOT WixBundleInstalled" Cancel="yes" /></Button>',
    '    </Page>',
    '    <Page Name="Progress">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes">#(loc.ProgressHeader)</Label>',
    '      <Label X="18" Y="121" Width="70" Height="17" FontId="3" DisablePrefix="yes">#(loc.ProgressLabel)</Label>',
    '      <Label Name="OverallProgressPackageText" X="92" Y="121" Width="-18" Height="17" FontId="3" DisablePrefix="yes">#(loc.OverallProgressPackageText)</Label>',
    '      <Progressbar Name="OverallCalculatedProgressbar" X="18" Y="143" Width="-18" Height="15" />',
    '      <Button Name="ProgressCancelButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0">#(loc.ProgressCancelButton)</Button>',
    '    </Page>',
    '    <Page Name="Modify">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes">#(loc.ModifyHeader)</Label>',
    '      <Label X="18" Y="121" Width="-18" Height="34" FontId="3" DisablePrefix="yes">Choose Components, then Repair to apply additions, removals, or verified updates.</Label>',
    '      <Button Name="ModifyOptionsButton" X="18" Y="165" Width="145" Height="24" TabStop="yes" FontId="0"><Text>Choose Components</Text><ChangePageAction Page="Options" /></Button>',
    '      <Button Name="ModifyUpdateButton" X="18" Y="-14" Width="210" Height="24" TabStop="yes" FontId="0" EnableCondition="WixStdBAUpdateAvailable" HideWhenDisabled="yes">#(loc.UpdateButton)</Button>',
    '      <Button Name="RepairButton" X="-188" Y="-14" Width="86" Height="24" TabStop="yes" FontId="0" HideWhenDisabled="yes" ElevationShieldCondition="WixBundleAuthoredScope = 1 AND NOT WixBundleElevated">#(loc.ModifyRepairButton)</Button>',
    '      <Button Name="UninstallButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0" ElevationShieldCondition="WixBundleAuthoredScope = 1 AND NOT WixBundleElevated">#(loc.ModifyUninstallButton)</Button>',
    '      <Button Name="ModifyCancelButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.ModifyCancelButton)</Text><CloseWindowAction /></Button>',
    '    </Page>',
    '    <Page Name="Success">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes"><Text>#(loc.SuccessHeader)</Text><Text Condition="WixBundleAction = 2">#(loc.SuccessLayoutHeader)</Text><Text Condition="WixBundleAction = 3">#(loc.SuccessUnsafeUninstallHeader)</Text><Text Condition="WixBundleAction = 4">#(loc.SuccessUninstallHeader)</Text><Text Condition="WixBundleAction = 5">#(loc.SuccessCacheHeader)</Text><Text Condition="WixBundleAction = 6">#(loc.SuccessInstallHeader)</Text><Text Condition="WixBundleAction = 7">#(loc.SuccessModifyHeader)</Text><Text Condition="WixBundleAction = 8">#(loc.SuccessRepairHeader)</Text></Label>',
    '      <Button Name="LaunchButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0" HideWhenDisabled="yes">#(loc.SuccessLaunchButton)</Button>',
    '      <Label X="-18" Y="-54" Width="450" Height="34" FontId="3" DisablePrefix="yes" VisibleCondition="WixStdBARestartRequired"><Text>#(loc.SuccessRestartText)</Text><Text Condition="WixBundleAction = 3">#(loc.SuccessUninstallRestartText)</Text></Label>',
    '      <Button Name="SuccessRestartButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0" HideWhenDisabled="yes">#(loc.SuccessRestartButton)</Button>',
    '      <Button Name="SuccessCloseButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.SuccessCloseButton)</Text><CloseWindowAction /></Button>',
    '    </Page>',
    '    <Page Name="Failure">',
    '      <Label X="18" Y="80" Width="-18" Height="30" FontId="2" DisablePrefix="yes"><Text>#(loc.FailureHeader)</Text><Text Condition="WixBundleAction = 2">#(loc.FailureLayoutHeader)</Text><Text Condition="WixBundleAction = 3">#(loc.FailureUnsafeUninstallHeader)</Text><Text Condition="WixBundleAction = 4">#(loc.FailureUninstallHeader)</Text><Text Condition="WixBundleAction = 5">#(loc.FailureCacheHeader)</Text><Text Condition="WixBundleAction = 6">#(loc.FailureInstallHeader)</Text><Text Condition="WixBundleAction = 7">#(loc.FailureModifyHeader)</Text><Text Condition="WixBundleAction = 8">#(loc.FailureRepairHeader)</Text></Label>',
    '      <Hypertext Name="FailureLogFileLink" X="18" Y="121" Width="-18" Height="42" FontId="3" TabStop="yes" HideWhenDisabled="yes">#(loc.FailureHyperlinkLogText)</Hypertext>',
    '      <Hypertext Name="FailureMessageText" X="28" Y="163" Width="-18" Height="51" FontId="3" TabStop="yes" HideWhenDisabled="yes" />',
    '      <Label X="-18" Y="-54" Width="450" Height="34" FontId="3" DisablePrefix="yes" VisibleCondition="WixStdBARestartRequired">#(loc.FailureRestartText)</Label>',
    '      <Button Name="FailureRestartButton" X="-98" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0" HideWhenDisabled="yes">#(loc.FailureRestartButton)</Button>',
    '      <Button Name="FailureCloseButton" X="-18" Y="-14" Width="80" Height="24" TabStop="yes" FontId="0"><Text>#(loc.FailureCloseButton)</Text><CloseWindowAction /></Button>',
    '    </Page>',
    '  </Window>',
    '</Theme>',
    '',
  ].join("\n");
}

function quoted(value) {
  return `&quot;${value}&quot;`;
}

export function buildWindowsSuiteBundleSource(options) {
  const validated = validateInputs(options);
  const {
    media, msi, baFunctions, setup, entries, policy, selectedScope, defaults,
  } = validated;
  const featureArguments = policy.features
    .map((feature) => `--bundle-feature ${feature.component_id}=[${feature.variable}]`)
    .join(" ");
  const restart = selectedScope.restart_manager_service
    ? " --restart-manager-service" : "";
  const layout = buildWindowsWixLayoutProjection({
    policy: readJson(fileURLToPath(new URL("../../packaging/install-layout.json", import.meta.url)), "InstallLayout policy"),
    scope: options.scope,
  });
  const roots = Object.fromEntries(Object.entries(layout.roots).map(([name, root]) => [
    name, `[${root.relative_to === "integration" ? "ArenaProgramRoot" : "ArenaDataRoot"}]\\${root.expression}`,
  ]));
  const pathArguments = [
    `--install-root ${quoted(roots.install)}`,
    `--data-root ${quoted("[ArenaDataRoot]")}`,
    `--cache-root ${quoted(roots.cache)}`,
    `--workspace-root ${quoted(roots.workspace)}`,
    `--knowledge-root ${quoted(roots.knowledge)}`,
  ];
  const global = [
    `--install-scope ${options.scope}`,
    `--bundle-owner ${quoted("[WixBundleProviderKey]")}`,
    ...pathArguments,
    `--catalog ${quoted("[WixBundleExecutePackageCacheFolder]release\\catalog.json")}`,
    options.development
      ? "--allow-unsigned-development-catalog"
      : `--metadata-dir ${quoted("[WixBundleExecutePackageCacheFolder]release\\tuf")}`,
  ].filter(Boolean).join(" ");
  const transport = options.offline ? " --offline" : "";
  const install = `${global}${restart} --result-file ${quoted("[WixBundleLog_SuiteComponents].install.json")} upgrade --yes${transport} ${featureArguments}`;
  const repair = `${global}${restart} --result-file ${quoted("[WixBundleLog_SuiteComponents].repair.json")} repair --yes${transport} ${featureArguments}`;
  const uninstall = [
    `--install-scope ${options.scope}`,
    `--bundle-owner ${quoted("[WixBundleProviderKey]")}`,
    ...pathArguments,
    `--result-file ${quoted("[WixBundleLog_SuiteComponents].uninstall.json")}`,
    "uninstall --all --yes",
  ].join(" ");
  const setupRelative = path.normalize(path.join("bin", "arena-setup.exe"));
  const payloads = entries.filter((relative) => path.normalize(relative) !== setupRelative);
  const variables = policy.features.map((feature) =>
    `    <Variable Name="${xml(feature.variable)}" Type="numeric" Value="${defaults.has(feature.component_id) ? 1 : 0}" Persisted="yes" bal:Overridable="yes" />`);
  variables.push(
    `    <Variable Name="ArenaProgramRoot" Type="formatted" Value="${xml(layout.anchors.integration.expression)}" Persisted="yes" bal:Overridable="yes" />`,
    `    <Variable Name="ArenaDataRoot" Type="formatted" Value="${xml(layout.anchors.data.expression)}" Persisted="yes" bal:Overridable="yes" />`,
  );
  const payloadLines = payloads.map((relative, index) =>
    `        <Payload Id="SuiteMedia${String(index + 1).padStart(4, "0")}" SourceFile="${xml(path.join(media, relative))}" Name="${xml(payloadName(relative).replaceAll("/", "\\"))}" />`);
  const name = `${policy.display_name} ${selectedScope.name_suffix}`
    + (options.offline ? " Offline" : "");
  return {
    source: [
      '<?xml version="1.0" encoding="UTF-8"?>',
      '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs" xmlns:bal="http://wixtoolset.org/schemas/v4/wxs/bal">',
      `  <Bundle Name="${xml(name)}" Manufacturer="${xml(policy.manufacturer)}" Version="${xml(options.version)}" UpgradeCode="${xml(selectedScope.upgrade_code)}" IconSourceFile="${xml(path.resolve(options.iconPath))}">`,
      '    <BootstrapperApplication>',
      `      <bal:WixStandardBootstrapperApplication Theme="hyperlinkLicense" ThemeFile="${xml(path.resolve(options.themePath))}" LocalizationFile="${xml(path.resolve(options.localizationPath))}" LicenseUrl="" SuppressOptionsUI="no" />`,
      `      <Payload Id="ArenaBundleBAFunctions" SourceFile="${xml(baFunctions)}" bal:BAFunctions="yes" />`,
      '    </BootstrapperApplication>',
      ...variables,
      '    <Chain>',
      `      <MsiPackage Id="ManagerCore" SourceFile="${xml(msi)}" Visible="no" Vital="yes">`,
      '        <MsiProperty Name="INSTALL_ROOT" Value="[ArenaProgramRoot]" />',
      '        <MsiProperty Name="ARENA_DATA_ROOT" Value="[ArenaDataRoot]" />',
      '      </MsiPackage>',
      `      <ExePackage Id="SuiteComponents" SourceFile="${xml(setup)}" InstallArguments="${install}" RepairArguments="${repair}" UninstallArguments="${uninstall}" DetectCondition="WixBundleInstalled" Protocol="none" PerMachine="${selectedScope.per_machine ? "yes" : "no"}" Permanent="no" Vital="yes">`,
      ...payloadLines,
      '      </ExePackage>',
      '    </Chain>',
      '  </Bundle>',
      '</Wix>',
      '',
    ].join("\n"),
    theme: buildSuiteTheme(policy.features),
  };
}

export function buildWindowsSuiteBundle(options) {
  const output = path.resolve(options.output);
  const parsedOutput = path.parse(output);
  const staging = path.join(
    parsedOutput.dir, `${parsedOutput.name}.staging-${process.pid}${parsedOutput.ext}`,
  );
  const generated = buildWindowsSuiteBundleSource(options);
  mkdirSync(path.dirname(output), { recursive: true });
  const temporary = path.join(
    os.tmpdir(), `uci-arena-suite-bundle-${process.pid}-${Date.now()}`,
  );
  mkdirSync(temporary, { recursive: false });
  const sourcePath = path.join(temporary, "suite.wxs");
  const themePath = path.join(temporary, "suite-theme.xml");
  try {
    writeFileSync(sourcePath, generated.source);
    writeFileSync(themePath, generated.theme);
    const rebuilt = buildWindowsSuiteBundleSource({ ...options, themePath });
    writeFileSync(sourcePath, rebuilt.source);
    rmSync(staging, { force: true });
    const result = spawnSync(options.wix ?? "wix", [
      "build", "-arch", "x64",
      "-ext", "WixToolset.BootstrapperApplications.wixext",
      "-o", staging, sourcePath,
    ], { encoding: "utf8", windowsHide: true });
    if (result.error || result.status !== 0) {
      const diagnostics = [result.error?.message, result.stderr, result.stdout]
        .filter((value) => typeof value === "string" && value.trim().length > 0)
        .join("\n");
      throw new Error(`WiX suite bundle build failed: ${diagnostics || `exit ${result.status}`}`);
    }
    rmSync(output, { force: true });
    renameSync(staging, output);
    return { output, size_bytes: statSync(output).size };
  } finally {
    rmSync(staging, { force: true });
    rmSync(temporary, { recursive: true, force: true });
  }
}

function main() {
  const args = parse(process.argv.slice(2));
  for (const required of [
    "media-root", "manager-msi", "policy", "version", "scope", "icon",
    "localization", "output",
  ]) {
    if (!args[required]) throw new Error(`missing --${required}`);
  }
  const development = args.development === "true";
  if (args.offline !== undefined && !["true", "false"].includes(args.offline)) {
    throw new Error("--offline must be true or false");
  }
  const offline = args.offline === "true";
  process.stdout.write(`${JSON.stringify(buildWindowsSuiteBundle({
    mediaRoot: args["media-root"],
    managerMsi: args["manager-msi"],
    baFunctionsPath: args["ba-functions"],
    policyPath: args.policy,
    version: args.version,
    scope: args.scope,
    iconPath: args.icon,
    localizationPath: args.localization,
    themePath: args.theme ?? args.localization,
    output: args.output,
    wix: args.wix,
    development,
    offline,
  }))}\n`);
}

if (path.resolve(process.argv[1] ?? "") === fileURLToPath(import.meta.url)) {
  main();
}
