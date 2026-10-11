#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  lstatSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import {
  normalizeComponentPath,
  validateExtractedComponent,
} from "./component-artifact.mjs";
import { validateInstallerRuntime } from "./build-installer-runtime.mjs";
import { buildWindowsWixLayoutProjection } from "./install-layout.mjs";

const VERSION_PATTERN = /^\d+\.\d+\.\d+$/u;
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

function stableId(prefix, value) {
  return `${prefix}_${createHash("sha256").update(value).digest("hex").slice(0, 24)}`;
}

function stableGuid(value) {
  const bytes = createHash("sha256").update(value).digest().subarray(0, 16);
  bytes[6] = (bytes[6] & 0x0f) | 0x50;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = bytes.toString("hex").toUpperCase();
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function validatePolicy(policy, scope) {
  if (policy?.schema !== "arena_manager_windows_integration_v1"
      || policy.component_id !== "manager.core"
      || typeof policy.installer_runtime_root !== "string"
      || typeof policy.setup_launcher_destination !== "string") {
    throw new Error("invalid Manager Windows integration policy identity");
  }
  const selected = policy.scopes?.[scope];
  if (!selected || !Array.isArray(selected.root_segments)
      || selected.root_segments.length === 0
      || selected.root_segments.some((segment) =>
        typeof segment !== "string" || segment.length === 0 || /[\\/]/u.test(segment))) {
    throw new Error(`invalid Manager Windows integration scope: ${scope}`);
  }
  if (scope === "per-user" && selected.service !== null) {
    throw new Error("per-user Manager integration must not register a service");
  }
  if (scope === "per-machine" && !selected.service?.name) {
    throw new Error("per-machine Manager integration must declare its service");
  }
  return selected;
}

function validateManagerArtifact(managerRoot, version, policy) {
  const validated = validateExtractedComponent({
    root: managerRoot,
    componentId: policy.component_id,
    version,
    target: "windows-x86_64",
  });
  const { manifest, declared } = validated;

  for (const [role, policyKey] of Object.entries(policy.entrypoints)) {
    const relative = normalizeComponentPath(
      manifest.entrypoints?.[policyKey], `Manager ${role} entrypoint`,
    );
    if (!declared.has(relative)) {
      throw new Error(`Manager ${role} entrypoint is not manifest-owned: ${relative}`);
    }
  }
  return validated;
}

function directoryTree(paths) {
  const root = { children: new Map() };
  for (const relative of paths) {
    const segments = path.posix.dirname(relative) === "."
      ? [] : path.posix.dirname(relative).split("/");
    let node = root;
    let current = "";
    for (const segment of segments) {
      current = current ? `${current}/${segment}` : segment;
      if (!node.children.has(segment)) {
        node.children.set(segment, { relative: current, children: new Map() });
      }
      node = node.children.get(segment);
    }
  }
  return root;
}

function renderDirectories(node, indent = "      ") {
  const lines = [];
  for (const [name, child] of [...node.children.entries()].sort()) {
    lines.push(`${indent}<Directory Id="${stableId("DIR", child.relative)}" Name="${xml(name)}">`);
    lines.push(...renderDirectories(child, `${indent}  `));
    lines.push(`${indent}</Directory>`);
  }
  return lines;
}

function directoryId(relative) {
  const parent = path.posix.dirname(relative);
  return parent === "." ? "INSTALL_ROOT" : stableId("DIR", parent);
}

function renderFileComponent({
  relative,
  source,
  scope,
  selectedScope,
  policy,
  manifest,
  layout,
}) {
  const componentId = stableId("CMP", `${scope}:${relative}`);
  const componentGuid = stableGuid(`uci-arena-manager:${scope}:${relative}`);
  const fileId = stableId("FILE", relative);
  const lines = [
    `      <Component Id="${componentId}" Directory="${directoryId(relative)}" Guid="${componentGuid}">`,
    `        <File Id="${fileId}" Source="${xml(source)}" Name="${xml(path.posix.basename(relative))}"${scope === "per-machine" ? ' KeyPath="yes"' : ""}>`,
  ];
  const desktopPath = manifest.entrypoints[policy.entrypoints.desktop];
  if (relative === desktopPath) {
    const child = (name) => layout.roots[name].relative_to === "integration"
      ? `[INSTALL_ROOT]${layout.roots[name].expression}`
      : `[ARENA_DATA_ROOT]\\${layout.roots[name].expression}`;
    const arguments_ = `--data-dir &quot;[ARENA_DATA_ROOT]&quot; --install-root &quot;${child("install")}&quot; --cache-root &quot;${child("cache")}&quot; --workspace-root &quot;${child("workspace")}&quot; --knowledge-root &quot;${child("knowledge")}&quot;`;
    lines.push(
      `          <Shortcut Id="ArenaManagerStartMenuShortcut" Directory="ProgramMenuFolder" Name="${xml(policy.start_menu.name)}" WorkingDirectory="INSTALL_ROOT" Arguments="${arguments_}" Advertise="no" />`,
    );
  }
  lines.push("        </File>");

  const cliPath = manifest.entrypoints[policy.entrypoints.cli];
  if (relative === cliPath) {
    lines.push(
      `        <Environment Id="ArenaManagerPath" Name="PATH" Value="[INSTALL_ROOT]${xml(policy.path.relative_directory)}" Action="set" Part="last" Permanent="no" System="${selectedScope.environment_system ? "yes" : "no"}" />`,
    );
  }

  const servicePath = manifest.entrypoints[policy.entrypoints.service];
  if (scope === "per-machine" && relative === servicePath) {
    const service = selectedScope.service;
    lines.push(
      `        <ServiceInstall Id="ArenaManagerServiceInstall" Name="${xml(service.name)}" DisplayName="${xml(service.display_name)}" Description="${xml(service.description)}" Account="${xml(service.account)}" Type="ownProcess" Start="auto" ErrorControl="normal" Arguments="${service.arguments}" />`,
      `        <ServiceControl Id="ArenaManagerServiceControl" Name="${xml(service.name)}" Start="install" Stop="both" Remove="uninstall" Wait="yes" />`,
    );
  }

  if (scope === "per-user") {
    lines.push(
      `        <RegistryValue Root="HKCU" Key="Software\\UCI Arena\\Installer\\Components" Name="${componentId}" Type="integer" Value="1" KeyPath="yes" />`,
    );
  }
  lines.push("      </Component>");
  return { componentId, lines };
}

export function buildManagerMsiSource({
  managerRoot,
  managerVersion,
  installerRoot,
  installerVersion,
  setupLauncher,
  scopeMarker,
  policyPath,
  version,
  scope,
  iconPath,
}) {
  if (!VERSION_PATTERN.test(version ?? "") || !SCOPES.has(scope)) {
    throw new Error("Manager MSI requires a three-field version and fixed scope");
  }
  const resolvedRoot = path.resolve(managerRoot);
  const resolvedInstallerRoot = path.resolve(installerRoot);
  const resolvedSetupLauncher = path.resolve(setupLauncher);
  const resolvedMarker = path.resolve(scopeMarker);
  const resolvedIcon = path.resolve(iconPath);
  for (const [label, filename] of [
    ["Manager component root", resolvedRoot],
    ["Installer runtime root", resolvedInstallerRoot],
    ["setup launcher", resolvedSetupLauncher],
    ["scope marker", resolvedMarker],
    ["installer icon", resolvedIcon],
  ]) {
    if (!lstatSync(filename)[label.endsWith("root") ? "isDirectory" : "isFile"]()) {
      throw new Error(`${label} is unavailable: ${filename}`);
    }
  }

  const policy = readJson(path.resolve(policyPath), "Manager Windows integration policy");
  const scopeIntegration = validatePolicy(policy, scope);
  const layout = buildWindowsWixLayoutProjection({
    policy: readJson(fileURLToPath(new URL("../../packaging/install-layout.json", import.meta.url)), "InstallLayout policy"),
    scope,
  });
  const selectedScope = {
    ...scopeIntegration,
    root_folder: layout.anchors.integration.wix_folder_id,
    root_segments: layout.anchors.integration.segments,
    data_root: layout.anchors.data.expression,
  };
  const { manifest, declared, manifestPath } = validateManagerArtifact(
    resolvedRoot, managerVersion ?? version, policy,
  );
  const sources = new Map(declared);
  sources.set("arena-component.json", manifestPath);
  const runtime = validateInstallerRuntime({
    root: resolvedInstallerRoot,
    version: installerVersion,
    target: "windows-x86_64",
  });
  const runtimeRootRelative = normalizeComponentPath(
    policy.installer_runtime_root, "Installer runtime destination",
  );
  const runtimeDestination = (relative) => normalizeComponentPath(
    path.posix.join(runtimeRootRelative, relative), "Installer runtime file destination",
  );
  const runtimeManifestDestination = runtimeDestination("arena-installer-runtime.json");
  const launcherDestination = normalizeComponentPath(
    policy.setup_launcher_destination, "setup launcher destination",
  );
  const markerRelative = normalizeComponentPath(policy.scope_marker, "scope marker path");
  const setupHeader = readFileSync(runtime.declared.get(runtime.entrypoint)).subarray(0, 2);
  if (setupHeader.length !== 2 || setupHeader[0] !== 0x4d || setupHeader[1] !== 0x5a) {
    throw new Error("Installer setup executable is not a Windows PE image");
  }
  const launcherHeader = readFileSync(resolvedSetupLauncher).subarray(0, 2);
  if (launcherHeader.length !== 2 || launcherHeader[0] !== 0x4d || launcherHeader[1] !== 0x5a) {
    throw new Error("Installer setup launcher is not a Windows PE image");
  }
  if (!new RegExp(`^${scope}\\r?\\n?$`, "u").test(readFileSync(resolvedMarker, "utf8"))) {
    throw new Error(`scope marker must contain exactly ${scope}`);
  }
  for (const [relative, filename] of runtime.declared) {
    const destination = runtimeDestination(relative);
    if (sources.has(destination)) {
      throw new Error(`Installer runtime destination collision: ${destination}`);
    }
    sources.set(destination, filename);
  }
  for (const [destination, filename] of [
    [runtimeManifestDestination, runtime.manifestPath],
    [launcherDestination, resolvedSetupLauncher],
    [markerRelative, resolvedMarker],
  ]) {
    if (sources.has(destination)) {
      throw new Error(`Installer integration destination collision: ${destination}`);
    }
    sources.set(destination, filename);
  }
  const payloadPaths = [...sources.keys()].sort();
  const components = payloadPaths.map((relative) => renderFileComponent({
    relative,
    source: sources.get(relative),
    scope,
    selectedScope,
    policy,
    manifest,
    layout,
  }));
  const rootOpen = selectedScope.root_segments.map((segment, index) => {
    const relative = selectedScope.root_segments.slice(0, index + 1).join("/");
    const id = index === selectedScope.root_segments.length - 1
      ? "INSTALL_ROOT" : stableId("ROOT", relative);
    return `${"      ".repeat(index + 1)}<Directory Id="${id}" Name="${xml(segment)}">`;
  });
  const rootClose = [...rootOpen].reverse().map((_, index) =>
    `${"      ".repeat(selectedScope.root_segments.length - index)}</Directory>`);
  const childDirectories = renderDirectories(directoryTree(payloadPaths),
    "      ".repeat(selectedScope.root_segments.length + 1));
  const packageScope = selectedScope.package_scope;
  const layoutKey = `Software\\UCI Arena\\Installer\\PackageLayout\\${scope}`;
  const layoutHive = scope === "per-user" ? "HKCU" : "HKLM";
  const layoutComponent = stableId("CMP", `package-layout:${scope}`);
  const layoutSearches = [
    ["ARENA_PRIOR_SCHEMA", "Schema"], ["ARENA_PRIOR_SCOPE", "Scope"],
    ["ARENA_PRIOR_INTEGRATION", "IntegrationRoot"], ["ARENA_PRIOR_DATA", "DataRoot"],
  ].map(([property, name]) => `    <Property Id="${property}" Secure="yes"><RegistrySearch Id="Search${property}" Root="${layoutHive}" Key="${xml(layoutKey)}" Name="${name}" Type="raw" Bitness="always64" /></Property>`);

  return [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
    `  <Package Name="${xml(policy.display_name)}" Version="${xml(version)}" Manufacturer="${xml(policy.manufacturer)}" UpgradeCode="${xml(selectedScope.upgrade_code)}" Scope="${packageScope}" InstallerVersion="500" Language="1033" Compressed="yes">`,
    '    <MediaTemplate EmbedCab="yes" />',
    '    <MajorUpgrade Schedule="afterInstallInitialize" DowngradeErrorMessage="A later version of [ProductName] is already installed." />',
    `    <Upgrade Id="${xml(selectedScope.opposite_upgrade_code)}">`,
    '      <UpgradeVersion Property="OPPOSITE_SCOPE_FOUND" Minimum="0.0.0" IncludeMinimum="yes" OnlyDetect="yes" />',
    '    </Upgrade>',
    '    <Launch Condition="NOT OPPOSITE_SCOPE_FOUND OR Installed" Message="The other UCI Arena Manager installation scope is already installed. Uninstall it before changing scope." />',
    '    <Property Id="ARENA_DATA_ROOT" Secure="yes" />',
    ...layoutSearches,
    '    <Property Id="INSTALL_ROOT" Secure="yes" />',
    '    <SetProperty Id="INSTALL_ROOT" Value="[ARENA_PRIOR_INTEGRATION]" Before="CostFinalize" Sequence="both" Condition="NOT INSTALL_ROOT AND ARENA_PRIOR_INTEGRATION" />',
    '    <SetProperty Id="ARENA_DATA_ROOT" Action="RestoreArenaDataRoot" Value="[ARENA_PRIOR_DATA]" Before="SetARENA_DATA_ROOT" Sequence="both" Condition="NOT ARENA_DATA_ROOT AND ARENA_PRIOR_DATA" />',
    `    <SetProperty Id="ARENA_DATA_ROOT" Value="${xml(layout.anchors.data.expression)}" Before="CostFinalize" Sequence="both" Condition="NOT ARENA_DATA_ROOT" />`,
    '    <Launch Condition="NOT (ARENA_PRIOR_SCHEMA OR ARENA_PRIOR_SCOPE OR ARENA_PRIOR_INTEGRATION OR ARENA_PRIOR_DATA) OR (ARENA_PRIOR_SCHEMA = &quot;arena_package_layout_selection_v1&quot; AND ARENA_PRIOR_SCOPE = &quot;' + scope + '&quot; AND ARENA_PRIOR_INTEGRATION AND ARENA_PRIOR_DATA)" Message="The retained UCI Arena path selection is incomplete or unsupported. Repair that installation before reinstalling." />',
    '    <CustomAction Id="RejectChangedPackageRoots" Error="The installed UCI Arena paths cannot be changed by repair or upgrade. Uninstall first to select another location." />',
    '    <InstallUISequence><Custom Action="RejectChangedPackageRoots" After="CostFinalize" Condition="NOT REMOVE AND ((ARENA_PRIOR_INTEGRATION AND NOT INSTALL_ROOT ~= ARENA_PRIOR_INTEGRATION) OR (ARENA_PRIOR_DATA AND NOT ARENA_DATA_ROOT ~= ARENA_PRIOR_DATA))" /></InstallUISequence>',
    '    <InstallExecuteSequence><Custom Action="RejectChangedPackageRoots" After="CostFinalize" Condition="NOT REMOVE AND ((ARENA_PRIOR_INTEGRATION AND NOT INSTALL_ROOT ~= ARENA_PRIOR_INTEGRATION) OR (ARENA_PRIOR_DATA AND NOT ARENA_DATA_ROOT ~= ARENA_PRIOR_DATA))" /></InstallExecuteSequence>',
    '    <Property Id="ARPPRODUCTICON" Value="ArenaInstallerIcon" />',
    `    <Icon Id="ArenaInstallerIcon" SourceFile="${xml(resolvedIcon)}" />`,
    '    <StandardDirectory Id="ProgramMenuFolder" />',
    `    <StandardDirectory Id="${xml(selectedScope.root_folder)}">`,
    ...rootOpen,
    ...childDirectories,
    ...rootClose,
    '    </StandardDirectory>',
    '    <Feature Id="ManagerCore" Title="UCI Arena Manager" Level="1">',
    ...components.map(({ componentId }) => `      <ComponentRef Id="${componentId}" />`),
    `      <ComponentRef Id="${layoutComponent}" />`,
    '    </Feature>',
    ...components.flatMap(({ lines }) => lines),
    `    <Component Id="${layoutComponent}" Directory="INSTALL_ROOT" Guid="${stableGuid(`uci-arena-package-layout:${scope}`)}" Bitness="always64">`,
    `      <RegistryKey Root="${layoutHive}" Key="${xml(layoutKey)}">`,
    '        <RegistryValue Name="Schema" Type="string" Value="arena_package_layout_selection_v1" KeyPath="yes" />',
    `        <RegistryValue Name="Scope" Type="string" Value="${scope}" />`,
    '        <RegistryValue Name="IntegrationRoot" Type="string" Value="[INSTALL_ROOT]" />',
    '        <RegistryValue Name="DataRoot" Type="string" Value="[ARENA_DATA_ROOT]" />',
    '      </RegistryKey>',
    '    </Component>',
    '  </Package>',
    '</Wix>',
    '',
  ].join("\n");
}

export function buildManagerMsi(options) {
  const source = buildManagerMsiSource(options);
  const output = path.resolve(options.output);
  const parsedOutput = path.parse(output);
  const staging = path.join(
    parsedOutput.dir, `${parsedOutput.name}.staging-${process.pid}${parsedOutput.ext}`,
  );
  mkdirSync(path.dirname(output), { recursive: true });
  const temporary = path.join(
    os.tmpdir(), `uci-arena-manager-msi-${process.pid}-${Date.now()}`,
  );
  mkdirSync(temporary, { recursive: false });
  const sourcePath = path.join(temporary, "manager.wxs");
  try {
    writeFileSync(sourcePath, source);
    rmSync(staging, { force: true });
    const result = spawnSync(options.wix ?? "wix", [
      "build", "-arch", "x64", "-pdbtype", "none", "-o", staging, sourcePath,
    ], { encoding: "utf8", windowsHide: true });
    if (result.error || result.status !== 0) {
      const diagnostics = [result.error?.message, result.stderr, result.stdout]
        .filter((value) => typeof value === "string" && value.trim().length > 0)
        .join("\n");
      throw new Error(
        `WiX Manager MSI build failed: ${diagnostics || `exit ${result.status}`}`,
      );
    }
    rmSync(output, { force: true });
    renameSync(staging, output);
    return { output, source_sha256: createHash("sha256").update(source).digest("hex") };
  } finally {
    rmSync(staging, { force: true });
    rmSync(temporary, { recursive: true, force: true });
  }
}

function main() {
  const args = parse(process.argv.slice(2));
  for (const required of [
    "manager-root", "installer-root", "installer-version", "setup-launcher", "scope-marker",
    "policy", "version", "scope", "icon", "output",
  ]) {
    if (!args[required]) throw new Error(`missing --${required}`);
  }
  process.stdout.write(`${JSON.stringify(buildManagerMsi({
    managerRoot: args["manager-root"],
    managerVersion: args["manager-version"] ?? args.version,
    installerRoot: args["installer-root"],
    installerVersion: args["installer-version"],
    setupLauncher: args["setup-launcher"],
    scopeMarker: args["scope-marker"],
    policyPath: args.policy,
    version: args.version,
    scope: args.scope,
    iconPath: args.icon,
    output: args.output,
    wix: args.wix,
  }))}\n`);
}

if (path.resolve(process.argv[1] ?? "") === fileURLToPath(import.meta.url)) {
  main();
}
