#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  realpathSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import { main as assembleWindowsLauncher } from "./assemble-windows-installer-launcher.mjs";
import { buildInstallerRuntime } from "./build-installer-runtime.mjs";
import { buildLinuxPortableInstaller } from "./build-linux-portable-installer.mjs";
import { buildManagerLinuxPackage } from "./build-manager-linux-package.mjs";
import { buildManagerMsi } from "./build-manager-msi.mjs";
import { buildWindowsSuiteBundle } from "./build-windows-suite-bundle.mjs";
import { validateExtractedComponent } from "./component-artifact.mjs";
import { canonicalTargetTriple, verifyComponentCandidate } from "./component-provenance.mjs";
import { admitCandidateAssets } from "./candidate-assets.mjs";
import { validateCompositionManifests } from "./product-composition.mjs";
import { verifyManagedCandidate } from "./managed-provenance.mjs";
import {
  resolveCosignExecutable,
  verifyProviderCandidate,
} from "./provider-provenance.mjs";
import {
  loadReleaseInputs,
  loadReleaseValidationOptions,
  sha256File,
  validateReleaseLock,
} from "./release-lock.mjs";
import { buildSuiteCatalog } from "./suite-catalog.mjs";

const REPOSITORY = "iteathen/uci-arena-installer";
const repositoryRoot = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const TRANSIENT_RENAME_ERROR_CODES = new Set(["EACCES", "EBUSY", "EPERM"]);

function waitSynchronously(milliseconds) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds);
}

export function renameWithTransientRetry(source, destination, {
  rename = renameSync,
  wait = waitSynchronously,
  attempts = 12,
} = {}) {
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    try {
      rename(source, destination);
      return;
    } catch (error) {
      const retryable = TRANSIENT_RENAME_ERROR_CODES.has(error?.code);
      if (!retryable || attempt + 1 >= attempts) throw error;
      wait(Math.min(50 * (2 ** attempt), 500));
    }
  }
}

function readJson(filename, label) {
  try {
    const status=lstatSync(filename);
    if(!status.isFile()||status.isSymbolicLink()||status.size<1||status.size>16777216)throw Error('bounded regular JSON required');
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is not valid JSON: ${error.message}`);
  }
}

function runChecked(executable, args, { cwd = repositoryRoot, environment = process.env } = {}) {
  const result = spawnSync(executable, args, {
    cwd,
    env: environment,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    const diagnostic = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim().length > 0)
      .join("\n");
    throw new Error(
      `${executable} ${args.slice(0, 4).join(" ")} failed: ${diagnostic || `exit ${result.status}`}`,
    );
  }
  return String(result.stdout ?? "").trim();
}

export const windowsLauncherDryRunTimeoutMilliseconds = 120_000;

export function validateWindowsLauncherEnvelope(filename, {
  execute = spawnSync,
  timeoutMilliseconds = windowsLauncherDryRunTimeoutMilliseconds,
} = {}) {
  if (!Number.isSafeInteger(timeoutMilliseconds)
      || timeoutMilliseconds < 1_000 || timeoutMilliseconds > 300_000) {
    throw new Error("Windows launcher envelope-validation timeout policy is invalid");
  }
  const result = execute(filename, ["--validate-envelope"], {
    cwd: path.dirname(filename),
    encoding: "utf8",
    windowsHide: true,
    timeout: timeoutMilliseconds,
  });
  if (result.error || result.status !== 0) {
    const diagnostic = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim().length > 0)
      .join("\n");
    throw new Error(
      `Windows installer launcher envelope validation failed: ${diagnostic || `exit ${result.status}`}`,
    );
  }
}

export function resolveNpmInvocation({
  platform = process.platform,
  execPath = process.execPath,
  environment = process.env,
  fileExists = existsSync,
} = {}) {
  if (platform !== "win32") return { executable: "npm", argsPrefix: [] };

  const windowsPath = path.win32;
  const candidates = [
    environment.ARENA_NPM_CLI_PATH,
    environment.npm_execpath,
    windowsPath.join(windowsPath.dirname(execPath), "node_modules", "npm", "bin", "npm-cli.js"),
    ...String(environment.PATH ?? "")
      .split(";")
      .filter((entry) => entry.length > 0)
      .map((entry) => windowsPath.join(entry, "node_modules", "npm", "bin", "npm-cli.js")),
  ];
  const npmCli = candidates.find((candidate) => typeof candidate === "string"
    && windowsPath.isAbsolute(candidate)
    && fileExists(candidate));
  if (!npmCli) {
    throw new Error(
      "npm CLI is unavailable; install npm beside Node.js or set ARENA_NPM_CLI_PATH to npm-cli.js",
    );
  }
  return { executable: execPath, argsPrefix: [windowsPath.resolve(npmCli)] };
}

export function sourceIdentity() {
  const status = runChecked("git", ["status", "--porcelain", "--untracked-files=no"]);
  if (status !== "") throw new Error("clean build requires a clean tracked worktree");
  const commit = runChecked("git", ["rev-parse", "HEAD"]);
  if (!/^[0-9a-f]{40}$/u.test(commit)) throw new Error("source commit identity is invalid");
  const epoch = Number(runChecked("git", ["show", "-s", "--format=%ct", "HEAD"]));
  if (!Number.isSafeInteger(epoch) || epoch < 0) throw new Error("source epoch is invalid");
  return { commit, epoch };
}

function assetIndex(root) {
  const resolvedRoot = path.resolve(root);
  if (!lstatSync(resolvedRoot).isDirectory()) {
    throw new Error(`candidate asset root is unavailable: ${resolvedRoot}`);
  }
  const index = new Map();
  const visit = (directory) => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const filename = path.join(directory, entry.name);
      const status = lstatSync(filename);
      if (status.isSymbolicLink()) throw new Error(`candidate asset is a symbolic link: ${filename}`);
      if (status.isDirectory()) visit(filename);
      else if (status.isFile()) {
        if (index.has(entry.name)) throw new Error(`candidate asset name is ambiguous: ${entry.name}`);
        index.set(entry.name, filename);
      } else {
        throw new Error(`candidate asset has unsupported type: ${filename}`);
      }
    }
  };
  visit(resolvedRoot);
  return index;
}

function selectRow(rows, componentId, target) {
  const candidates = rows.filter((row) => row.component_id === componentId
    && (row.target_triple === target
      || row.target_triple === "any" || row.target_triple === "any-any"));
  if (candidates.length !== 1) {
    throw new Error(`${componentId} has ${candidates.length} candidates for ${target}`);
  }
  return candidates[0];
}

export function selectLockedComponents(lock, composition, target) {
  return composition.components.map((component) => {
    const current=lock.schema==='arena_suite_release_lock_v3';
    const managed = component.repository === REPOSITORY;
    const row = selectRow(
      current?lock.components:(managed ? lock.managed_components : lock.components),
      component.component_id,
      target,
    );
    return { component, lockRow: row, managed:current?row.ownership==='installer_managed':managed };
  });
}

function candidateDirectoryMap(candidateDirectories, selected) {
  if(!candidateDirectories||typeof candidateDirectories!=='object'||Array.isArray(candidateDirectories)
    ||Object.keys(candidateDirectories).length!==selected.length
    ||selected.some(({component})=>!Object.hasOwn(candidateDirectories,component.component_id)))throw Error('exact per-row candidate directories are required');
  for(const value of Object.values(candidateDirectories))if(typeof value!=='string'||!path.isAbsolute(value))throw Error('candidate directories must be explicit absolute paths');
  return candidateDirectories;
}

function v3Validation(options){
  const {lock,composition,validationOptions,trustPolicyFilename}=options;
  const bound=loadReleaseValidationOptions({lock,compositionFileName:validationOptions?.compositionFileName,
    compositionSha256:validationOptions?.compositionSha256},{...validationOptions,trustPolicyFilename});
  const checked=validateReleaseLock(lock,composition,bound);
  if(!checked.ok)throw Error(checked.errors.join('\n'));
  return bound;
}

export async function verifyV3Candidates({lock,composition,target,candidateDirectories,
  validationOptions,trustPolicyFilename,cosignExecutable}) {
  validationOptions=v3Validation({lock,composition,validationOptions,trustPolicyFilename});
  const selected=selectLockedComponents(lock,composition,target),directories=candidateDirectoryMap(candidateDirectories,selected);
  const result=[];
  for(const selection of selected){
    const verified=await verifyComponentCandidate({row:selection.lockRow,component:selection.component,
      candidateDirectory:directories[selection.component.component_id],trustPolicy:validationOptions.trustPolicy,
      // The existing common port's parameter is named Directory, but its keyed
      // adapter consumes the actual policy FILE path to resolve public keys.
      trustPolicyDirectory:trustPolicyFilename,cosignPath:cosignExecutable,validationMode:validationOptions.validationMode});
    if(verified.artifact.type!=='archive')throw Error('current complete-suite assembly requires archive candidates');
    result.push({...selection,verified,artifactPath:path.join(directories[verified.component_id],verified.artifact.name)});
  }
  return result;
}

async function snapshotV3Directories({lock,composition,target,candidateDirectories,workRoot}){
  const selected=selectLockedComponents(lock,composition,target),directories=candidateDirectoryMap(candidateDirectories,selected),snapshots={};
  for(const {component,lockRow} of selected){
    const assets=await admitCandidateAssets({row:lockRow,candidateDirectory:directories[component.component_id]});
    const output=path.join(workRoot,'candidate-bytes',component.component_id);mkdirSync(output,{recursive:true});
    copyFileSync(assets.artifact.path,path.join(output,lockRow.artifact.name));
    writeFileSync(path.join(output,lockRow.evidence.name),assets.evidence.bytes,{flag:'wx'});
    writeFileSync(path.join(output,lockRow.provenance.bundle_name),assets.bundle.bytes,{flag:'wx'});
    snapshots[component.component_id]=output;
  }
  return snapshots;
}

async function verifyAndExtractV3Candidates(options){
  // Identity, ownership, target, dependencies and trust fail before even staging.
  options={...options,validationOptions:v3Validation(options)};
  const snapshots=await snapshotV3Directories(options);
  // Verify EVERY immutable staged row before extracting any archive.
  const admitted=await verifyV3Candidates({...options,candidateDirectories:snapshots});
  const candidates=[];
  for(const {component,lockRow,verified,artifactPath} of admitted){
    const extractedRoot=path.join(options.workRoot,'extracted',component.component_id);
    mkdirSync(path.dirname(extractedRoot),{recursive:true});extractArchive(artifactPath,extractedRoot);
    const manifestPath=path.join(extractedRoot,'arena-component.json');
    if(sha256File(manifestPath)!==verified.component_manifest_sha256)throw Error('component manifest digest differs from verified provenance');
    const manifest=readJson(manifestPath,'component manifest');
    if(canonicalTargetTriple(manifest.target_triple)!==verified.target_triple)throw Error('component manifest canonical target differs');
    const extracted=validateExtractedComponent({root:extractedRoot,componentId:component.component_id,
      version:component.version,target:manifest.target_triple});
    candidates.push({repository:verified.source_repository,lockRow,verified,artifactPath,
      artifactSizeBytes:verified.artifact.size_bytes,manifest:extracted.manifest,
      manifestSizeBytes:statSync(extracted.manifestPath).size,extractedRoot});
  }
  validateCompositionManifests(options.composition,candidates.map(c=>({component_id:c.verified.component_id,
    version:c.verified.component_version,repository:c.repository,manifest:c.manifest})));
  return candidates;
}

function extractArchive(archive, destination) {
  mkdirSync(destination, { recursive: false });
  runChecked("cmake", [
    "-E", "chdir", destination,
    "cmake", "-E", "tar", "xf", archive,
  ]);
}

export function verifyAndExtractCandidates({
  lock,
  composition,
  target,
  assetsRoot,
  workRoot,
  cosignExecutable,
  candidateDirectories,validationOptions,trustPolicyFilename,
}) {
  if(lock.schema==='arena_suite_release_lock_v3')return verifyAndExtractV3Candidates({lock,composition,target,
    candidateDirectories,validationOptions,trustPolicyFilename,workRoot,cosignExecutable});
  const assets = assetIndex(assetsRoot);
  const selected = selectLockedComponents(lock, composition, target);
  return selected.map(({ component, lockRow, managed }) => {
    const artifactPath = assets.get(lockRow.artifact_name);
    const resultName = managed ? lockRow.managed_result_name : lockRow.provider_result_name;
    const resultPath = assets.get(resultName);
    const bundlePath = assets.get(lockRow.provenance_bundle_name);
    if (!artifactPath || !resultPath || !bundlePath) {
      throw new Error(`${component.component_id} locked candidate bytes are incomplete`);
    }
    let verified;
    if (managed) {
      verified = verifyManagedCandidate({
        artifactPath,
        managedResultPath: resultPath,
        bundlePath,
        lockRow,
        cosignExecutable,
      });
    } else {
      verified = verifyProviderCandidate({
        artifactPath,
        providerResultPath: resultPath,
        bundlePath,
        lockRow,
        provider: lock.providers[lockRow.provider_id],
        cosignExecutable,
      });
    }
    if ((lockRow.artifact_type ?? "archive") !== "archive") {
      throw new Error(
        `${component.component_id} uses a file-set artifact; the current composition requires archives`,
      );
    }
    const extractedRoot = path.join(workRoot, "extracted", component.component_id);
    mkdirSync(path.dirname(extractedRoot), { recursive: true });
    extractArchive(artifactPath, extractedRoot);
    const extracted = validateExtractedComponent({
      root: extractedRoot,
      componentId: component.component_id,
      version: component.version,
      target: lockRow.target_triple,
    });
    const result = managed ? verified.managed_result : verified.provider_result;
    if (sha256File(extracted.manifestPath) !== result.component_manifest_sha256) {
      throw new Error(`${component.component_id} manifest digest differs from its release result`);
    }
    return {
      repository: component.repository,
      lockRow,
      providerResult: result,
      artifactPath,
      artifactSizeBytes: statSync(artifactPath).size,
      manifest: extracted.manifest,
      manifestSizeBytes: statSync(extracted.manifestPath).size,
      extractedRoot,
    };
  });
}

function copyTree(sourceRoot, destinationRoot) {
  const visit = (source, destination) => {
    mkdirSync(destination, { recursive: true });
    for (const entry of readdirSync(source, { withFileTypes: true })) {
      const sourcePath = path.join(source, entry.name);
      const destinationPath = path.join(destination, entry.name);
      const status = lstatSync(sourcePath);
      if (status.isSymbolicLink()) throw new Error(`build input is a symbolic link: ${sourcePath}`);
      if (status.isDirectory()) visit(sourcePath, destinationPath);
      else if (status.isFile()) copyFileSync(sourcePath, destinationPath);
      else throw new Error(`build input has unsupported type: ${sourcePath}`);
    }
  };
  visit(path.resolve(sourceRoot), path.resolve(destinationRoot));
}

function installerVersionFromCache(buildRoot) {
  const cache = readFileSync(path.join(buildRoot, "CMakeCache.txt"), "utf8");
  const match = cache.match(/^CMAKE_PROJECT_VERSION:STATIC=([^\r\n]+)$/mu);
  if (!match || !/^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/u.test(match[1])) {
    throw new Error("configured Installer version is unavailable");
  }
  return match[1];
}

function buildInstallerSource({ platform, installTree }) {
  const preset = platform === "windows-x86_64" ? "windows-release" : "linux-release";
  const buildRoot = path.join(repositoryRoot, "build", preset);
  const environment = { ...process.env };
  if (!environment.VCPKG_ROOT) {
    const localVcpkg = path.join(repositoryRoot, "build", "tooling", "vcpkg");
    if (!existsSync(path.join(localVcpkg, "scripts", "buildsystems", "vcpkg.cmake"))) {
      throw new Error("VCPKG_ROOT and repository-local vcpkg are unavailable");
    }
    environment.VCPKG_ROOT = localVcpkg;
  }
  const npm = resolveNpmInvocation({ environment });
  runChecked(npm.executable, [...npm.argsPrefix, "run", "check"], { environment });
  runChecked("cmake", ["--preset", preset], { environment });
  runChecked("cmake", ["--build", "--preset", preset], { environment });
  runChecked("ctest", ["--preset", preset], { environment });
  const installArgs = ["--install", buildRoot];
  if (platform === "windows-x86_64") installArgs.push("--config", "Release");
  installArgs.push("--prefix", installTree);
  runChecked("cmake", installArgs, { environment });
  return { preset, buildRoot, installerVersion: installerVersionFromCache(buildRoot) };
}

function writeScopeMarker(root, scope) {
  const filename = path.join(root, `${scope}.txt`);
  writeFileSync(filename, `${scope}\n`);
  return filename;
}

function artifactRecord(filename) {
  const status = lstatSync(filename);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`clean build artifact is invalid: ${filename}`);
  }
  return {
    name: path.basename(filename),
    size_bytes: status.size,
    sha256: sha256File(filename),
  };
}

export function cleanBuildArtifactNames(platform, suiteVersion) {
  if (!/^\d+\.\d+\.\d+$/u.test(suiteVersion ?? "")) {
    throw new Error("clean build requires a three-field suite version");
  }
  if (platform === "windows-x86_64") {
    return [
      `UCI-Arena-Setup-${suiteVersion}-windows-x64-online.exe`,
      `UCI-Arena-Setup-${suiteVersion}-windows-x64-offline.exe`,
      `UCI-Arena-Manager-${suiteVersion}-windows-x64-user.msi`,
      `UCI-Arena-Manager-${suiteVersion}-windows-x64-machine.msi`,
    ];
  }
  if (platform === "linux-x86_64") {
    return [
      `uci-arena-setup-${suiteVersion}-linux-x86_64-online.run`,
      `uci-arena-setup-${suiteVersion}-linux-x86_64-offline.run`,
      `uci-arena-manager_${suiteVersion}_amd64.deb`,
      `uci-arena-manager-${suiteVersion}-1.x86_64.rpm`,
      `uci-arena-manager-${suiteVersion}-linux-x86_64.tar.gz`,
    ];
  }
  throw new Error(`unsupported clean-build platform: ${platform}`);
}

function buildSuiteMedia({
  composition,
  candidates,
  nativeBuild,
  installTree,
  sourceEpoch,
  workRoot,
  target,
  featureComponentIds,
}) {
  const runtimeRoot = path.join(workRoot, "installer-runtime");
  buildInstallerRuntime({
    installTree,
    outputRoot: runtimeRoot,
    version: nativeBuild.installerVersion,
    target,
    sourceDateEpoch: sourceEpoch,
  });
  const mediaRoot = path.join(workRoot, "media");
  copyTree(runtimeRoot, mediaRoot);
  const payloadRoot = path.join(mediaRoot, "release", "payloads");
  mkdirSync(payloadRoot, { recursive: true });
  for (const candidate of candidates) {
    copyFileSync(
      candidate.artifactPath,
      path.join(payloadRoot, candidate.verified?.artifact.name??candidate.lockRow.artifact_name),
    );
  }
  const catalog = buildSuiteCatalog({
    composition,
    target,
    candidates,
    featureComponentIds,
  });
  mkdirSync(path.join(mediaRoot, "release"), { recursive: true });
  writeFileSync(
    path.join(mediaRoot, "release", "catalog.json"),
    `${JSON.stringify(catalog, null, 2)}\n`,
  );
  const managerComponent = composition.components.find(
    (component) => component.role === "manager",
  );
  const manager = candidates.find(
    (candidate) => managerComponent?.component_id === candidate.lockRow.component_id,
  );
  if (!manager) throw new Error("validated Manager component is unavailable");
  return { runtimeRoot, mediaRoot, manager };
}

function buildWindowsOutputs({
  composition,
  candidates,
  nativeBuild,
  installTree,
  sourceEpoch,
  workRoot,
  publishRoot,
}) {
  const suiteVersion = composition.product_release;
  const windowsPolicyPath = path.join(repositoryRoot, "packaging", "windows", "suite-bundle.json");
  const windowsPolicy = readJson(windowsPolicyPath, "Windows suite bundle policy");
  const { runtimeRoot, mediaRoot, manager } = buildSuiteMedia({
    composition,
    candidates,
    nativeBuild,
    installTree,
    sourceEpoch,
    workRoot,
    target: "windows-x86_64",
    featureComponentIds: windowsPolicy.features.map((feature) => feature.component_id),
  });
  const [onlineName, offlineName, userMsiName, machineMsiName] =
    cleanBuildArtifactNames("windows-x86_64", suiteVersion);
  const markerRoot = path.join(workRoot, "scope-markers");
  mkdirSync(markerRoot, { recursive: true });
  const userMarker = writeScopeMarker(markerRoot, "per-user");
  const machineMarker = writeScopeMarker(markerRoot, "per-machine");
  const iconPath = path.join(repositoryRoot, "packaging", "icons", "uci-arena-installer.ico");
  const managerPolicy = path.join(
    repositoryRoot, "packaging", "windows", "manager-integration.json",
  );
  const setupLauncher = path.join(
    nativeBuild.buildRoot, "Release", "arena-setup-launcher.exe",
  );
  const baFunctionsPath = path.join(
    nativeBuild.buildRoot, "Release", "arena-bundle-bafunctions.dll",
  );
  const userMsi = path.join(publishRoot, userMsiName);
  const machineMsi = path.join(publishRoot, machineMsiName);
  for (const [scope, marker, output] of [
    ["per-user", userMarker, userMsi],
    ["per-machine", machineMarker, machineMsi],
  ]) {
    buildManagerMsi({
      managerRoot: manager.extractedRoot,
      managerVersion: manager.lockRow.component_version,
      installerRoot: runtimeRoot,
      installerVersion: nativeBuild.installerVersion,
      setupLauncher,
      scopeMarker: marker,
      policyPath: managerPolicy,
      version: suiteVersion,
      scope,
      iconPath,
      output,
    });
  }
  const childRoot = path.join(workRoot, "child-bundles");
  mkdirSync(childRoot, { recursive: true });
  const localizationPath = path.join(
    repositoryRoot, "packaging", "windows", "suite-theme.en-us.wxl",
  );
  const children = {};
  for (const offline of [false, true]) {
    const transport = offline ? "offline" : "online";
    for (const [scope, managerMsi] of [
      ["per-user", userMsi], ["per-machine", machineMsi],
    ]) {
      const output = path.join(childRoot, `${scope}-${transport}.exe`);
      buildWindowsSuiteBundle({
        mediaRoot,
        managerMsi,
        baFunctionsPath,
        policyPath: windowsPolicyPath,
        version: suiteVersion,
        scope,
        iconPath,
        localizationPath,
        themePath: localizationPath,
        output,
        development: true,
        offline,
      });
      children[`${scope}-${transport}`] = output;
    }
  }
  const stub = path.join(
    nativeBuild.buildRoot,
    "Release",
    "uci-arena-installer-launcher.exe",
  );
  const online = path.join(publishRoot, onlineName);
  const offline = path.join(publishRoot, offlineName);
  assembleWindowsLauncher([
    "--stub", stub,
    "--personal", children["per-user-online"],
    "--all-users", children["per-machine-online"],
    "--output", online,
  ]);
  assembleWindowsLauncher([
    "--stub", stub,
    "--personal", children["per-user-offline"],
    "--all-users", children["per-machine-offline"],
    "--output", offline,
  ]);
  validateWindowsLauncherEnvelope(online);
  validateWindowsLauncherEnvelope(offline);
  return [online, offline, userMsi, machineMsi].map(artifactRecord);
}

function buildLinuxOutputs({
  composition,
  candidates,
  nativeBuild,
  installTree,
  sourceEpoch,
  workRoot,
  publishRoot,
}) {
  const suiteVersion = composition.product_release;
  const { mediaRoot, manager } = buildSuiteMedia({
    composition,
    candidates,
    nativeBuild,
    installTree,
    sourceEpoch,
    workRoot,
    target: "linux-x86_64",
    featureComponentIds: composition.components.map((component) => component.component_id),
  });
  const [onlineName, offlineName, debName, rpmName, tarName] =
    cleanBuildArtifactNames("linux-x86_64", suiteVersion);
  const policyPath = path.join(
    repositoryRoot, "packaging", "linux", "manager-integration.json",
  );
  for (const [format, name] of [
    ["deb", debName],
    ["rpm", rpmName],
    ["tar", tarName],
  ]) {
    buildManagerLinuxPackage({
      managerRoot: manager.extractedRoot,
      policyPath,
      managerVersion: manager.lockRow.component_version,
      version: suiteVersion,
      format,
      output: path.join(publishRoot, name),
      sourceDateEpoch: sourceEpoch,
    });
  }
  for (const [offline, name] of [
    [false, onlineName],
    [true, offlineName],
  ]) {
    buildLinuxPortableInstaller({
      mediaRoot,
      output: path.join(publishRoot, name),
      sourceDateEpoch: sourceEpoch,
      offline,
      development: true,
    });
  }
  return [onlineName, offlineName, debName, rpmName, tarName]
    .map((name) => artifactRecord(path.join(publishRoot, name)));
}

function canonicalDirectory(filename){
  const fold=value=>process.platform==='win32'?value.toLowerCase():value;
  if(lstatSync(filename).isSymbolicLink()||fold(realpathSync(filename))!==fold(path.resolve(filename)))throw Error('output ownership path is linked');
}

export function removeOwnedTree(ownerRoot,target){
  const root=path.resolve(ownerRoot),resolved=path.resolve(target),relative=path.relative(root,resolved);
  if(!relative||relative.startsWith('..')||path.isAbsolute(relative))throw Error('cleanup target escapes task-owned root');
  canonicalDirectory(root);
  if(existsSync(resolved)){canonicalDirectory(resolved);rmSync(resolved,{recursive:true,force:true});}
}

export function transactionalOutput(outputRoot, producer) {
  const output=path.resolve(outputRoot),parent=path.dirname(output);
  if(output===path.parse(output).root)throw Error('clean build output cannot be a filesystem root');
  mkdirSync(parent,{recursive:true});canonicalDirectory(parent);
  if(existsSync(output)){canonicalDirectory(output);if(!lstatSync(output).isDirectory())throw Error('output must be a directory');}
  const transaction=mkdtempSync(path.join(parent,`.${path.basename(output)}.transaction-`));
  canonicalDirectory(transaction);
  const previous=path.join(transaction,'previous');
  const publish = path.join(transaction, "publish");
  const work = path.join(transaction, "work");
  mkdirSync(publish);
  mkdirSync(work);
  let retain=false;
  const cleanup=()=>{if(!retain)removeOwnedTree(parent,transaction);};
  const commit=result=>{
    const hadPrevious = existsSync(output);
    canonicalDirectory(transaction);canonicalDirectory(publish);canonicalDirectory(parent);
    if(hadPrevious){canonicalDirectory(output);renameWithTransientRetry(output,previous);}
    try {
      renameWithTransientRetry(publish, output);
    } catch (error) {
      if (hadPrevious && existsSync(previous)) {
        canonicalDirectory(previous);
        if(existsSync(output)){retain=true;throw Error(`output changed during rollback; retained ${transaction}`);}
        renameWithTransientRetry(previous, output);
      }
      throw error;
    }
    if(hadPrevious){try{removeOwnedTree(transaction,previous);}catch(error){retain=true;throw Error(`output committed; previous output retained at ${transaction}: ${error.message}`);}}
    return result;
  };
  const failed=error=>{try{cleanup();}catch(disposal){throw new AggregateError([error,disposal],`${error.message}; cleanup failed; retained ${transaction}`,{cause:error});}throw error;};
  const finished=result=>{let committed;try{committed=commit(result);}catch(error){return failed(error);}cleanup();return committed;};
  let result;try{result=producer({transactionRoot:transaction,publishRoot:publish,workRoot:work});}catch(error){return failed(error);}
  return result&&typeof result.then==='function'?Promise.resolve(result).then(finished,failed):finished(result);
}

function componentInputRows(lock, composition, target) {
  return selectLockedComponents(lock, composition, target).map(
    ({ component, lockRow, managed }) => ({
      component_id: component.component_id,
      component_version: component.version,
      repository: component.repository,
      target_triple: lockRow.target_triple,
      source_commit: lockRow.source_commit,
      workflow_ref:lockRow.provenance_workflow_ref,
      artifact_name: lockRow.artifact_name,
      artifact_sha256: lockRow.artifact_sha256,
      result_sha256: managed
        ? lockRow.managed_result_sha256 : lockRow.provider_result_sha256,
      provenance_bundle_sha256: lockRow.provenance_bundle_sha256,
    }),
  );
}

export async function cleanBuild({
  lockFilename,
  compositionDirectory,
  assetsRoot,
  outputRoot,
  platform,
  diagnosticReview = false,
  validationMode,trustPolicyFilename,candidateDirectoriesFilename,validateOnly=false,
}) {
  if (!["windows-x86_64", "linux-x86_64"].includes(platform)) {
    throw new Error("--platform must be windows-x86_64 or linux-x86_64");
  }
  if (platform === "linux-x86_64" && process.platform !== "linux") {
    throw new Error("Linux clean-build execution requires a Linux host");
  }
  const inputs = loadReleaseInputs(lockFilename, compositionDirectory);
  const validationOptions=loadReleaseValidationOptions(inputs,{
    requireReady: !diagnosticReview,
    requireNotReady: diagnosticReview,
    validationMode:validationMode??(inputs.lock.schema==='arena_suite_release_lock_v3'?undefined:(diagnosticReview?'diagnostic_review':'official')),
    trustPolicyFilename,
    requiredTargets: [platform],
    compositionFileName: inputs.compositionFileName,
    compositionSha256: inputs.compositionSha256,
  });
  const validation = validateReleaseLock(inputs.lock,inputs.composition,validationOptions);
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  const lockSha256 = sha256File(path.resolve(lockFilename));
  const cosignExecutable = resolveCosignExecutable({ repositoryRoot });
  const candidateDirectories=candidateDirectoriesFilename?readJson(candidateDirectoriesFilename,'candidate directory map'):undefined;
  if(validateOnly){
    if(inputs.lock.schema!=='arena_suite_release_lock_v3')throw Error('read-only input verification requires v3');
    const verified=await verifyV3Candidates({lock:inputs.lock,composition:inputs.composition,target:platform,
      candidateDirectories,validationOptions,trustPolicyFilename,cosignExecutable});
    return {schema:'arena_suite_inputs_verified_v1',release_lock_sha256:lockSha256,
      trust_policy_sha256:validationOptions.trustPolicySha256,validation_mode:validationOptions.validationMode,
      components:verified.map(row=>row.verified),built:false,extracted:false};
  }
  const source=sourceIdentity();
  return transactionalOutput(outputRoot, async ({ publishRoot, workRoot }) => {
    const candidates = await verifyAndExtractCandidates({
      lock: inputs.lock,
      composition: inputs.composition,
      target: platform,
      assetsRoot,
      workRoot,
      cosignExecutable,
      candidateDirectories,validationOptions,trustPolicyFilename,
    });
    const installTree = path.join(workRoot, "installer-install-tree");
    const nativeBuild = buildInstallerSource({ platform, installTree });
    const buildOutputs = platform === "windows-x86_64"
      ? buildWindowsOutputs : buildLinuxOutputs;
    const artifacts = buildOutputs({
      composition: inputs.composition,
      candidates,
      nativeBuild,
      installTree,
      sourceEpoch: source.epoch,
      workRoot,
      publishRoot,
    });
    const result = {
      schema: inputs.lock.schema==='arena_suite_release_lock_v3'?"arena_clean_build_result_v2":"arena_clean_build_result_v1",
      schema_version: inputs.lock.schema==='arena_suite_release_lock_v3'?2:1,
      suite_version: inputs.composition.product_release,
      target_platform: platform,
      source_repository: REPOSITORY,
      source_commit: source.commit,
      clean_tree: true,
      build_preset: nativeBuild.preset,
      input_authority: diagnosticReview
        ? "diagnostic_review_lock" : "release_ready_lock",
      composition_sha256: inputs.compositionSha256,
      release_lock_sha256: lockSha256,
      ...(inputs.lock.schema==='arena_suite_release_lock_v3'?{release_lock_schema:inputs.lock.schema,
        trust_policy_sha256:validationOptions.trustPolicySha256,validation_mode:validationOptions.validationMode}:{}),
      component_inputs: inputs.lock.schema==='arena_suite_release_lock_v3'?candidates.map(candidate=>candidate.verified):componentInputRows(inputs.lock, inputs.composition, platform),
      installer_authority: REPOSITORY,
      release_scope: "complete_development",
      release_classification: "unsigned_suite_candidate",
      artifacts,
    };
    writeFileSync(
      path.join(publishRoot, "clean-build-result.json"),
      `${JSON.stringify(result, null, 2)}\n`,
    );
    return result;
  });
}

function parse(argv) {
  const values = {};
  let completeDevelopment = false;
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === "--complete-development") {
      completeDevelopment = true;
      continue;
    }
    if(flag==='--validate-inputs'){if(values.validateOnly)throw Error('duplicate validate-inputs');values.validateOnly=true;continue;}
    if (!flag?.startsWith("--") || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    if(!['platform','lock','composition-directory','asset-root','output-root','diagnostic-review-lock',
      'validation-mode','trust-policy','candidate-directories'].includes(flag.slice(2))
      ||Object.hasOwn(values,flag.slice(2)))throw Error(`unknown or duplicate clean-build option ${flag}`);
    values[flag.slice(2)] = argv[++index];
  }
  if (!completeDevelopment) throw new Error("--complete-development is required");
  if (!values.platform) throw new Error("--platform is required");
  const diagnosticReview = Boolean(values["diagnostic-review-lock"]);
  if (diagnosticReview && values.lock) {
    throw new Error("use either --lock or --diagnostic-review-lock, not both");
  }
  const lockFilename = values["diagnostic-review-lock"]
    ?? values.lock ?? path.join(repositoryRoot, "release", "catalog.lock.json");
  const assetsRoot = values["asset-root"]
    ?? (diagnosticReview
      ? path.join(path.dirname(path.resolve(lockFilename)), "assets")
      : path.join(repositoryRoot, "build", "release-inputs"));
  return {
    lockFilename,
    compositionDirectory: values["composition-directory"]
      ?? path.join(repositoryRoot, "compositions"),
    assetsRoot,
    outputRoot: values["output-root"]
      ?? path.join(repositoryRoot, "dist", values.platform),
    platform: values.platform,
    diagnosticReview,
    validationMode:values['validation-mode'],
    trustPolicyFilename:values['trust-policy']?path.resolve(values['trust-policy']):undefined,
    candidateDirectoriesFilename:values['candidate-directories']?path.resolve(values['candidate-directories']):undefined,
    validateOnly:Boolean(values.validateOnly),
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    const result = await cleanBuild(parse(process.argv.slice(2)));
    process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
  } catch (error) {
    process.stderr.write(`clean-build: ${error.message}\n`);
    process.exitCode = 1;
  }
}
