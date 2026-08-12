import { existsSync, readFileSync, readdirSync } from "node:fs";
import path from "node:path";
import process from "node:process";

const SEMVER = /^[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?$/u;
const COMPONENT_ID = /^[a-z0-9][a-z0-9._-]*$/u;
const COMMIT = /^[0-9a-f]{40}$/u;

export function validateReleaseIntegration({
  releaseRef,
  localHead,
  remoteMainHead,
  allowUnmergedDiagnostic = false,
}) {
  if (!COMMIT.test(localHead ?? "") || !COMMIT.test(remoteMainHead ?? "")) {
    throw new Error("release commit identity is invalid");
  }
  if (releaseRef !== "main") {
    if (!allowUnmergedDiagnostic) {
      throw new Error("normal component releases require the exact integrated remote main head");
    }
    return { authority: "diagnostic_only", integrated_main: false };
  }
  if (localHead !== remoteMainHead) {
    throw new Error("local release head differs from integrated remote main");
  }
  return { authority: "integrated_main", integrated_main: true };
}

export function validateRunnerPathBudget({ runnerWorkRoot, repositorySlug }) {
  if (typeof runnerWorkRoot !== "string" || !runnerWorkRoot.trim()) {
    throw new Error("runner work root is unavailable");
  }
  const repositoryName = /^(?:[A-Za-z0-9_.-]+)\/([A-Za-z0-9_.-]+)$/u.exec(
    repositorySlug ?? "",
  )?.[1];
  if (!repositoryName) throw new Error("runner repository slug is invalid");
  const resolved = path.resolve(runnerWorkRoot);
  if (resolved === path.parse(resolved).root) {
    throw new Error("runner work root cannot be a filesystem root");
  }
  const common = [resolved, "0".repeat(16), "w", repositoryName, repositoryName];
  const projectedPaths = [
    path.join(
      ...common,
      "native", "build-repro", "CMakeFiles", "CMakeScratch",
      "TryCompile-abcdefgh", "cmTC_abcdefgh.dir", "Debug",
      "cmTC_abcdefgh.tlog", "link-cvtres.write.1.tlog",
    ),
    path.join(
      ...common,
      "vcpkg", "buildtrees", "libsodium", "x64-windows-release-rel",
      "builds", "msvc", "vs2022", "libsodium", "x64", "Release",
      "libsodium.dir", "Release", "libsodium-file-tracker.tlog",
      "link-cvtres.write.1.tlog",
    ),
  ];
  const projectedLength = Math.max(...projectedPaths.map((entry) => entry.length));
  if (projectedLength > 240) {
    throw new Error(`projected release build path exceeds the 240-character budget (${projectedLength})`);
  }
  return { runner_work_root: resolved, projected_path_characters: projectedLength };
}

export function validateCandidateReleaseTag({ candidateReleaseTag, expectedVersion }) {
  if (!candidateReleaseTag) return null;
  if (candidateReleaseTag.length > 200
      || !/^[A-Za-z0-9][A-Za-z0-9._/-]*$/u.test(candidateReleaseTag)
      || candidateReleaseTag.endsWith("/")
      || candidateReleaseTag.includes("..")) {
    throw new Error("candidate release tag is invalid");
  }
  if (!candidateReleaseTag.includes(`/v${expectedVersion}/`)) {
    throw new Error("candidate release tag is not bound to the requested version");
  }
  return candidateReleaseTag;
}

function readJson(filename, label) {
  try {
    return JSON.parse(readFileSync(filename, "utf8"));
  } catch (error) {
    throw new Error(`${label} is invalid: ${error.message}`);
  }
}

export function collectReleaseVersions(repositoryRoot) {
  const versions = [];
  const componentIds = [];
  const addVersion = (source, value) => {
    if (value === undefined) return;
    if (!SEMVER.test(value)) throw new Error(`${source} version is invalid`);
    versions.push({ source, value });
  };
  const addComponent = (source, value) => {
    if (value === undefined) return;
    if (!COMPONENT_ID.test(value)) throw new Error(`${source} component id is invalid`);
    componentIds.push({ source, value });
  };

  const packagePath = path.join(repositoryRoot, "package.json");
  if (existsSync(packagePath)) {
    const manifest = readJson(packagePath, "package.json");
    addVersion("package.json", manifest.version);
  }
  const componentPath = path.join(repositoryRoot, "contracts", "component-manifest.json");
  if (existsSync(componentPath)) {
    const manifest = readJson(componentPath, "component manifest");
    addVersion("contracts/component-manifest.json", manifest.component?.version
      ?? manifest.component_version);
    addComponent("contracts/component-manifest.json", manifest.component?.id
      ?? manifest.component_id);
  }
  const servicePath = path.join(repositoryRoot, "contracts", "service-contract.json");
  if (existsSync(servicePath)) {
    const contract = readJson(servicePath, "service contract");
    addVersion("contracts/service-contract.json", contract.service?.component_version
      ?? contract.component_version);
    addComponent("contracts/service-contract.json", contract.service?.component_id
      ?? contract.component_id);
  }
  const cmakePath = path.join(repositoryRoot, "CMakeLists.txt");
  if (existsSync(cmakePath)) {
    const source = readFileSync(cmakePath, "utf8");
    const match = source.match(/\bproject\s*\([\s\S]{0,512}?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?)/iu);
    if (match) addVersion("CMakeLists.txt", match[1]);
  }
  const componentDefinitionRoot = path.join(repositoryRoot, "release", "components");
  if (existsSync(componentDefinitionRoot)) {
    const definitions = readdirSync(componentDefinitionRoot, { withFileTypes: true })
      .filter((entry) => entry.isFile() && entry.name.endsWith(".json"))
      .map((entry) => entry.name)
      .sort();
    if (definitions.length > 64) throw new Error("component definition inventory is unbounded");
    for (const filename of definitions) {
      const definition = readJson(
        path.join(componentDefinitionRoot, filename),
        `release component definition ${filename}`,
      );
      addComponent(`release/components/${filename}`, definition.component_id);
    }
  }
  return { versions, componentIds };
}

function bindsDynamicWorkflowVersion(workflow) {
  const directInput = /^\s+RELEASE_VERSION:\s*\$\{\{\s*inputs\.version\s*\}\}\s*$/mu
    .test(workflow);
  const resolvedInput = /^\s+DISPATCH_VERSION:\s*\$\{\{\s*inputs\.version\s*\}\}\s*$/mu
    .test(workflow)
    && /^\s+version:\s*\$\{\{\s*steps\.release\.outputs\.version\s*\}\}\s*$/mu
      .test(workflow)
    && workflow.includes('version="${DISPATCH_VERSION}"')
    && workflow.includes("printf 'version=%s\\n' \"${version}\"")
    && /^\s+RELEASE_VERSION:\s*\$\{\{\s*needs\.resolve\.outputs\.version\s*\}\}\s*$/mu
      .test(workflow);
  return (directInput || resolvedInput)
    && workflow.includes("--version $env:RELEASE_VERSION")
    && workflow.includes("v$env:RELEASE_VERSION");
}

export function collectWorkflowDispatchInputs(workflow) {
  const lines = workflow.split(/\r?\n/u);
  const dispatchIndex = lines.findIndex((line) => /^ {2}workflow_dispatch:\s*$/u.test(line));
  if (dispatchIndex < 0) return new Set();
  let dispatchEnd = lines.length;
  for (let index = dispatchIndex + 1; index < lines.length; index += 1) {
    if (/^ {0,2}\S/u.test(lines[index])) {
      dispatchEnd = index;
      break;
    }
  }
  const inputsIndex = lines.findIndex(
    (line, index) => index > dispatchIndex && index < dispatchEnd
      && /^ {4}inputs:\s*$/u.test(line),
  );
  if (inputsIndex < 0) return new Set();
  const inputs = new Set();
  for (let index = inputsIndex + 1; index < dispatchEnd; index += 1) {
    const line = lines[index];
    if (/^ {0,4}\S/u.test(line)) break;
    const match = /^ {6}([A-Za-z_][A-Za-z0-9_-]*):\s*(?:#.*)?$/u.exec(line);
    if (match) inputs.add(match[1]);
  }
  return inputs;
}

export function validateReleaseSource({
  repositoryRoot,
  expectedVersion,
  expectedComponentId = "",
  workflowPath,
  requiredRunnerLabels,
  releaseRef,
  localHead,
  remoteMainHead,
  allowUnmergedDiagnostic = false,
  runnerWorkRoot,
  repositorySlug,
  candidateReleaseTag = "",
  forwardedWorkflowInputs = [],
}) {
  if (!SEMVER.test(expectedVersion ?? "")) throw new Error("expected version is invalid");
  const { versions, componentIds } = collectReleaseVersions(repositoryRoot);
  const workflow = readFileSync(workflowPath, "utf8");
  if (versions.length < 1) {
    if (!bindsDynamicWorkflowVersion(workflow)) {
      throw new Error("no authoritative component version source was found");
    }
    versions.push({
      source: "workflow_dispatch.inputs.version->ARENA_VERSION/artifact/tag",
      value: expectedVersion,
    });
  }
  const mismatches = versions.filter((entry) => entry.value !== expectedVersion);
  if (mismatches.length > 0) {
    throw new Error(`release version drift: ${mismatches.map(
      (entry) => `${entry.source}=${entry.value}`,
    ).join(", ")}; expected ${expectedVersion}`);
  }
  if (expectedComponentId) {
    if (!COMPONENT_ID.test(expectedComponentId)) throw new Error("expected component id is invalid");
    const selectedComponents = componentIds.filter(
      (entry) => entry.value === expectedComponentId,
    );
    if (selectedComponents.length < 1) {
      throw new Error(`release component identity does not declare ${expectedComponentId}`);
    }
  }
  if (!/^\s{2}workflow_dispatch:\s*$/mu.test(workflow)) {
    throw new Error("release workflow is not explicitly dispatchable");
  }
  const declaredWorkflowInputs = collectWorkflowDispatchInputs(workflow);
  if (new Set(forwardedWorkflowInputs).size !== forwardedWorkflowInputs.length) {
    throw new Error("forwarded workflow inputs must be unique");
  }
  for (const inputName of forwardedWorkflowInputs) {
    if (!/^[A-Za-z_][A-Za-z0-9_-]*$/u.test(inputName)) {
      throw new Error(`forwarded workflow input name is invalid: ${inputName}`);
    }
    if (!declaredWorkflowInputs.has(inputName)) {
      throw new Error(`release workflow does not declare forwarded input ${inputName}`);
    }
  }
  for (const forbidden of ["push", "pull_request", "pull_request_target", "schedule"]) {
    if (new RegExp(`^ {2}${forbidden}:`, "mu").test(workflow)) {
      throw new Error(`release workflow has forbidden automatic trigger: ${forbidden}`);
    }
  }
  if (!/^\s+id-token:\s*write\s*$/mu.test(workflow)) {
    throw new Error("release workflow does not grant OIDC signing authority");
  }
  if (!/\bcosign\s+(?:attest-blob|sign-blob)\b/u.test(workflow)
      || !/\bcosign\s+verify-(?:blob-attestation|blob)\b/u.test(workflow)) {
    throw new Error("release workflow does not contain the signing operation");
  }
  if (!/\brunner:\s*$/mu.test(workflow)
      || !workflow.includes("ephemeral-self-hosted-windows")) {
    throw new Error("release workflow has no explicit ephemeral runner input");
  }
  for (const label of requiredRunnerLabels) {
    if (!workflow.includes(`"${label}"`) && !workflow.includes(`'${label}'`)) {
      throw new Error(`release workflow is missing runner label ${label}`);
    }
  }
  const integration = validateReleaseIntegration({
    releaseRef,
    localHead,
    remoteMainHead,
    allowUnmergedDiagnostic,
  });
  const pathBudget = validateRunnerPathBudget({ runnerWorkRoot, repositorySlug });
  const releaseTag = validateCandidateReleaseTag({ candidateReleaseTag, expectedVersion });
  return {
    schema: "arena_local_release_preflight_v1",
    version: expectedVersion,
    component_id: expectedComponentId || null,
    version_sources: versions.length,
    runner_labels: requiredRunnerLabels.length,
    workflow: path.relative(repositoryRoot, workflowPath).split(path.sep).join("/"),
    candidate_release_tag: releaseTag,
    forwarded_workflow_inputs: forwardedWorkflowInputs.length,
    ...integration,
    ...pathBudget,
  };
}

function parse(argv) {
  const values = { label: [], "workflow-input": [] };
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === "--runner-label") values.label.push(argv[++index]);
    else if (flag === "--workflow-input") values["workflow-input"].push(argv[++index]);
    else if (flag.startsWith("--")) values[flag.slice(2)] = argv[++index];
    else throw new Error(`unexpected argument: ${flag}`);
  }
  if (!values.root || !values.version || !values.workflow || values.label.length < 1) {
    throw new Error("--root, --version, --workflow, and --runner-label are required");
  }
  const repositoryRoot = path.resolve(values.root);
  return {
    repositoryRoot,
    expectedVersion: values.version,
    expectedComponentId: values.component ?? "",
    workflowPath: path.resolve(repositoryRoot, values.workflow),
    requiredRunnerLabels: values.label,
    releaseRef: values.ref,
    localHead: values.head,
    remoteMainHead: values["remote-main-head"],
    allowUnmergedDiagnostic: values["allow-unmerged-diagnostic"] === "true",
    runnerWorkRoot: values["runner-work-root"],
    repositorySlug: values.repository,
    candidateReleaseTag: values["candidate-release-tag"] ?? "",
    forwardedWorkflowInputs: values["workflow-input"],
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    process.stdout.write(`${JSON.stringify(validateReleaseSource(parse(process.argv.slice(2))))}\n`);
  } catch (error) {
    process.stderr.write(`local-release-preflight: ${error.message}\n`);
    process.exitCode = 1;
  }
}
