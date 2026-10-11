import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  collectEphemeralRunnerLabelSets,
  collectWorkflowDispatchInputs,
  validateReleaseIntegration,
  validateCandidateReleaseTag,
  validatePublicationAuthority,
  validateRunnerPathBudget,
  validateReleaseSource,
  workflowPublicationAuthority,
} from "./local-release-preflight.mjs";

const HEAD = "0123456789abcdef0123456789abcdef01234567";
const RUNNER_ROOT = path.join(path.parse(path.resolve(".")).root, "ar");

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-release-preflight-"));
  mkdirSync(path.join(root, "contracts"));
  mkdirSync(path.join(root, ".github", "workflows"), { recursive: true });
  writeFileSync(path.join(root, "package.json"), JSON.stringify({ version: "1.2.3" }));
  writeFileSync(path.join(root, "contracts", "component-manifest.json"), JSON.stringify({
    component: { id: "provider.component", version: "1.2.3" },
  }));
  writeFileSync(path.join(root, "contracts", "service-contract.json"), JSON.stringify({
    service: { component_id: "provider.component", component_version: "1.2.3" },
  }));
  const labels = ["self-hosted", "Windows", "X64", "ephemeral", "uci-suite", "provider-release"];
  const workflowPath = path.join(root, ".github", "workflows", "component-release.yml");
  writeFileSync(workflowPath, [
    "name: release",
    "on:",
    "  workflow_dispatch:",
    "    inputs:",
    "      version:",
    "      target:",
    "      runner:",
    "        default: ephemeral-self-hosted-windows",
    "permissions:",
    "  id-token: write",
    "jobs:",
    "  release:",
    `    runs-on: \${{ fromJSON('[${labels.map((label) => `\"${label}\"`).join(",")}]') }}`,
    "    steps:",
    "      - run: cosign attest-blob artifact",
    "      - run: cosign verify-blob-attestation artifact",
    "      - run: gh release create component/provider/v1.2.3/windows-x86_64 artifact",
    "",
  ].join("\n"));
  return { root, workflowPath, labels };
}

test("release preflight binds versions, signing, dispatch, and runner labels", () => {
  const fixtureRoot = fixture();
  try {
    const result = validateReleaseSource({
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.3",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: fixtureRoot.labels,
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      publicationMode: "workflow-owned",
      forwardedWorkflowInputs: ["version", "target", "runner"],
    });
    assert.equal(result.version_sources, 3);
    assert.equal(result.runner_labels, 6);
    assert.equal(result.runner_count, 1);
    assert.deepEqual(result.runner_label_sets, [fixtureRoot.labels]);
  } finally {
    rmSync(fixtureRoot.root, { recursive: true, force: true });
  }
});

test("release preflight derives distinct Engine resolve and build runner labels", () => {
  const fixtureRoot = fixture();
  try {
    const base = ["self-hosted", "Windows", "X64", "ephemeral", "uci-suite"];
    const resolve = [...base, "engine-release-resolve"];
    const build = [...base, "engine-release-build"];
    const workflow = readFileSync(fixtureRoot.workflowPath, "utf8").replace(
      /^  release:[\s\S]*$/mu,
      [
        "  resolve:",
        `    runs-on: \${{ fromJSON('[${resolve.map((label) => `\"${label}\"`).join(",")}]') }}`,
        "    steps:",
        "      - run: cosign attest-blob artifact",
        "      - run: cosign verify-blob-attestation artifact",
        "  build:",
        `    runs-on: \${{ fromJSON('[${build.map((label) => `\"${label}\"`).join(",")}]') }}`,
        "    steps:",
        "      - run: cosign attest-blob artifact",
        "      - run: cosign verify-blob-attestation artifact",
        "      - run: gh release create component/provider/v1.2.3/windows-x86_64 artifact",
        "",
      ].join("\n"),
    );
    writeFileSync(fixtureRoot.workflowPath, workflow);
    assert.deepEqual(collectEphemeralRunnerLabelSets(workflow, base), [resolve, build]);
    const input = {
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.3",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: base,
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      publicationMode: "workflow-owned",
      requestedRunnerCount: 0,
    };
    const result = validateReleaseSource(input);
    assert.equal(result.runner_count, 2);
    assert.deepEqual(result.runner_label_sets, [resolve, build]);
    assert.throws(
      () => validateReleaseSource({ ...input, requestedRunnerCount: 1 }),
      /does not match workflow job count 2/u,
    );
  } finally {
    rmSync(fixtureRoot.root, { recursive: true, force: true });
  }
});

test("release preflight preserves one runner for each repeated job label set", () => {
  const base = ["self-hosted", "Windows", "X64", "ephemeral", "uci-suite"];
  const encoded = JSON.stringify([...base, "same-job"]);
  const workflow = [
    `    runs-on: \${{ fromJSON('${encoded}') }}`,
    `    runs-on: \${{ fromJSON('${encoded}') }}`,
  ].join("\n");
  assert.deepEqual(
    collectEphemeralRunnerLabelSets(workflow, base),
    [JSON.parse(encoded), JSON.parse(encoded)],
  );
});

test("release preflight rejects optional inputs absent from workflow dispatch", () => {
  const fixtureRoot = fixture();
  try {
    const workflow = readFileSync(fixtureRoot.workflowPath, "utf8");
    assert.deepEqual(
      [...collectWorkflowDispatchInputs(workflow)],
      ["version", "target", "runner"],
    );
    assert.throws(() => validateReleaseSource({
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.3",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: fixtureRoot.labels,
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      publicationMode: "workflow-owned",
      forwardedWorkflowInputs: ["version", "target", "runner", "component"],
    }), /does not declare forwarded input component/u);
  } finally {
    rmSync(fixtureRoot.root, { recursive: true, force: true });
  }
});

test("release preflight fails before dispatch on version or runner-label drift", () => {
  const fixtureRoot = fixture();
  try {
    assert.throws(() => validateReleaseSource({
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.4",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: fixtureRoot.labels,
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      publicationMode: "workflow-owned",
    }), /release version drift/u);
    assert.throws(() => validateReleaseSource({
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.3",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: [...fixtureRoot.labels, "missing-release-label"],
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      publicationMode: "workflow-owned",
    }), /missing runner label/u);
  } finally {
    rmSync(fixtureRoot.root, { recursive: true, force: true });
  }
});

test("normal releases require integrated remote main and branches are diagnostic-only", () => {
  assert.deepEqual(validateReleaseIntegration({
    releaseRef: "main",
    localHead: HEAD,
    remoteMainHead: HEAD,
  }), { authority: "integrated_main", integrated_main: true });
  assert.throws(() => validateReleaseIntegration({
    releaseRef: "agent/unmerged",
    localHead: HEAD,
    remoteMainHead: HEAD,
  }), /exact integrated remote main/u);
  assert.throws(() => validateReleaseIntegration({
    releaseRef: "main",
    localHead: HEAD,
    remoteMainHead: "1123456789abcdef0123456789abcdef01234567",
  }), /differs from integrated remote main/u);
  assert.deepEqual(validateReleaseIntegration({
    releaseRef: "agent/unmerged",
    localHead: HEAD,
    remoteMainHead: HEAD,
    allowUnmergedDiagnostic: true,
  }), { authority: "diagnostic_only", integrated_main: false });
});

test("dynamic Manager releases bind one dispatch version through build artifact and tag", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-dynamic-release-preflight-"));
  try {
    const workflowRoot = path.join(root, ".github", "workflows");
    const definitionRoot = path.join(root, "release", "components");
    mkdirSync(workflowRoot, { recursive: true });
    mkdirSync(definitionRoot, { recursive: true });
    writeFileSync(path.join(definitionRoot, "manager-core.json"), JSON.stringify({
      component_id: "manager.core",
    }));
    const workflowPath = path.join(workflowRoot, "component-release.yml");
    writeFileSync(workflowPath, [
      "on:",
      "  workflow_dispatch:",
      "    inputs:",
      "      version:",
      "      runner:",
      "        default: ephemeral-self-hosted-windows",
      "permissions:",
      "  id-token: write",
      "jobs:",
      "  release:",
      "    runs-on: ${{ fromJSON('[\"self-hosted\",\"Windows\",\"X64\",\"ephemeral\",\"uci-suite\",\"manager-release\"]') }}",
      "    env:",
      "      RELEASE_VERSION: ${{ inputs.version }}",
      "    steps:",
      "      - run: cmake --preset release -DARENA_VERSION=$env:RELEASE_VERSION",
      "      - run: node build.mjs --version $env:RELEASE_VERSION",
      "      - run: gh release create component/manager/v$env:RELEASE_VERSION",
      "      - run: cosign attest-blob artifact",
      "      - run: cosign verify-blob-attestation artifact",
      "",
    ].join("\n"));
    const result = validateReleaseSource({
      repositoryRoot: root,
      expectedVersion: "0.3.18",
      expectedComponentId: "manager.core",
      workflowPath,
      requiredRunnerLabels: [
        "self-hosted", "Windows", "X64", "ephemeral", "uci-suite", "manager-release",
      ],
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/manager",
      publicationMode: "workflow-owned",
    });
    assert.equal(result.version_sources, 1);
    assert.equal(result.component_id, "manager.core");
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("dynamic Engine releases bind a validated resolve-job version chain", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-resolved-release-preflight-"));
  try {
    const workflowRoot = path.join(root, ".github", "workflows");
    const definitionRoot = path.join(root, "release", "components");
    mkdirSync(workflowRoot, { recursive: true });
    mkdirSync(definitionRoot, { recursive: true });
    writeFileSync(path.join(definitionRoot, "engine.json"), JSON.stringify({
      component_id: "uci_arena.engine",
    }));
    writeFileSync(path.join(definitionRoot, "evidence.json"), JSON.stringify({
      component_id: "uci_arena_evidence.runtime",
    }));
    const workflowPath = path.join(workflowRoot, "component-release.yml");
    const workflow = [
      "on:",
      "  workflow_dispatch:",
      "    inputs:",
      "      version:",
      "      runner:",
      "        default: ephemeral-self-hosted-windows",
      "permissions:",
      "  id-token: write",
      "jobs:",
      "  resolve:",
      "    outputs:",
      "      version: ${{ steps.release.outputs.version }}",
      "    steps:",
      "      - id: release",
      "        env:",
      "          DISPATCH_VERSION: ${{ inputs.version }}",
      "        run: |",
      "          version=\"${DISPATCH_VERSION}\"",
      "          printf 'version=%s\\n' \"${version}\" >> \"${GITHUB_OUTPUT}\"",
      "  build:",
      "    runs-on: ${{ fromJSON('[\"self-hosted\",\"Windows\",\"X64\",\"ephemeral\",\"uci-suite\",\"engine-release\"]') }}",
      "    env:",
      "      RELEASE_VERSION: ${{ needs.resolve.outputs.version }}",
      "    steps:",
      "      - run: node build.mjs --version $env:RELEASE_VERSION",
      "      - run: gh release create component/engine/v$env:RELEASE_VERSION",
      "      - run: cosign attest-blob artifact",
      "      - run: cosign verify-blob-attestation artifact",
      "",
    ].join("\n");
    writeFileSync(workflowPath, workflow);
    const input = {
      repositoryRoot: root,
      expectedVersion: "0.1.11",
      expectedComponentId: "uci_arena.engine",
      workflowPath,
      requiredRunnerLabels: [
        "self-hosted", "Windows", "X64", "ephemeral", "uci-suite", "engine-release",
      ],
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/engine",
      publicationMode: "workflow-owned",
    };
    assert.equal(validateReleaseSource(input).version_sources, 1);
    assert.throws(() => validateReleaseSource({
      ...input,
      expectedComponentId: "undeclared.engine",
    }), /does not declare undeclared\.engine/u);

    writeFileSync(workflowPath, workflow.replace(
      "      version: ${{ steps.release.outputs.version }}\n",
      "",
    ));
    assert.throws(() => validateReleaseSource(input), /no authoritative component version/u);
    writeFileSync(workflowPath, workflow.replace(
      "node build.mjs --version $env:RELEASE_VERSION",
      "node build.mjs",
    ));
    assert.throws(() => validateReleaseSource(input), /no authoritative component version/u);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("runner preflight reserves an MSVC-safe build path budget", () => {
  const short = validateRunnerPathBudget({
    runnerWorkRoot: RUNNER_ROOT,
    repositorySlug: "iteathen/uci-arena-lichess-bot",
  });
  assert.equal(short.projected_path_characters <= 240, true);
  assert.throws(() => validateRunnerPathBudget({
    runnerWorkRoot: path.resolve("C:/", "long-release-root-".repeat(12)),
    repositorySlug: "iteathen/uci-arena-lichess-bot",
  }), /240-character budget/u);
  const evidenceLength = { runnerWorkRoot: RUNNER_ROOT, repositorySlug: "owner/uci-arena-evidence-service" };
  assert.throws(() => validateRunnerPathBudget(evidenceLength), /240-character budget/u);
  const nativeOnly = validateRunnerPathBudget({ ...evidenceLength, requiresVcpkg: false });
  assert.ok(Number.isFinite(nativeOnly.projected_path_characters));
  assert.ok(nativeOnly.projected_path_characters <= 240);
  assert.throws(() => validateRunnerPathBudget({ ...evidenceLength, requiresVcpkg: null }), /closure selection is invalid/u);
});

test("source runner path budget follows its declared native and vcpkg closure", () => {
  const owned = fixture();
  try {
    mkdirSync(path.join(owned.root, "native"));
    writeFileSync(path.join(owned.root, "native", "CMakeLists.txt"), "project(owned_native_helper)\n");
    const input = {
      repositoryRoot: owned.root, expectedVersion: "1.2.3", expectedComponentId: "provider.component",
      workflowPath: owned.workflowPath, requiredRunnerLabels: owned.labels,
      releaseRef: "main", localHead: HEAD, remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT, repositorySlug: "owner/uci-arena-evidence-service",
      publicationMode: "workflow-owned",
    };
    assert.ok(validateReleaseSource(input).projected_path_characters <= 240);
    assert.throws(() => validateReleaseSource({ ...input,
      runnerWorkRoot: path.join(RUNNER_ROOT, "long-native-release-root-".repeat(8)),
    }), /240-character budget/u);
    writeFileSync(path.join(owned.root, "vcpkg.json"), JSON.stringify({ dependencies: ["libsodium"] }));
    assert.throws(() => validateReleaseSource(input), /240-character budget/u);
  } finally {
    rmSync(owned.root, { recursive: true, force: true });
  }
});

test("source vcpkg configuration and selected workflow references retain deep projection", () => {
  for (const declaration of ["configuration", "workflow", "cmake", "native-manifest", "native-presets"]) {
    const owned = fixture();
    try {
      if (declaration === "configuration") writeFileSync(path.join(owned.root, "vcpkg-configuration.json"), "{}\n");
      if (declaration === "workflow") writeFileSync(owned.workflowPath,
        readFileSync(owned.workflowPath, "utf8") + "      - run: vcpkg install\n");
      if (declaration === "cmake") writeFileSync(path.join(owned.root, "CMakeLists.txt"),
        'set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")\n');
      if (declaration.startsWith("native-")) mkdirSync(path.join(owned.root, "native"));
      if (declaration === "native-manifest") writeFileSync(path.join(owned.root, "native", "vcpkg.json"), "{}\n");
      if (declaration === "native-presets") writeFileSync(path.join(owned.root, "native", "CMakePresets.json"),
        JSON.stringify({ configurePresets: [{ cacheVariables: { CMAKE_TOOLCHAIN_FILE: "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" } }] }));
      assert.throws(() => validateReleaseSource({
        repositoryRoot: owned.root, expectedVersion: "1.2.3", expectedComponentId: "provider.component",
        workflowPath: owned.workflowPath, requiredRunnerLabels: owned.labels,
        releaseRef: "main", localHead: HEAD, remoteMainHead: HEAD,
        runnerWorkRoot: RUNNER_ROOT, repositorySlug: "owner/uci-arena-evidence-service",
        publicationMode: "workflow-owned",
      }), /240-character budget/u);
    } finally {
      rmSync(owned.root, { recursive: true, force: true });
    }
  }
});

test("candidate prerelease tags are optional and version-bound", () => {
  assert.equal(validateCandidateReleaseTag({
    candidateReleaseTag: "",
    expectedVersion: "0.3.18",
  }), null);
  assert.equal(validateCandidateReleaseTag({
    candidateReleaseTag: "component/manager-core/v0.3.18/windows-x86_64",
    expectedVersion: "0.3.18",
  }), "component/manager-core/v0.3.18/windows-x86_64");
  assert.throws(() => validateCandidateReleaseTag({
    candidateReleaseTag: "component/manager-core/v0.3.17/windows-x86_64",
    expectedVersion: "0.3.18",
  }), /not bound to the requested version/u);
  assert.throws(() => validateCandidateReleaseTag({
    candidateReleaseTag: "../component/v0.3.18/windows-x86_64",
    expectedVersion: "0.3.18",
  }), /invalid/u);
});

test("release publication authority is explicit and internally consistent", () => {
  assert.throws(() => validatePublicationAuthority({
    publicationMode: "",
    candidateReleaseTag: "",
    expectedVersion: "1.0.19",
  }), /must be selected explicitly/u);
  assert.throws(() => validatePublicationAuthority({
    publicationMode: "implicit",
    candidateReleaseTag: "",
    expectedVersion: "1.0.19",
  }), /publication authority is invalid/u);
  assert.deepEqual(validatePublicationAuthority({
    publicationMode: "workflow-owned",
    candidateReleaseTag: "",
    expectedVersion: "1.0.19",
  }), {
    publication_mode: "workflow-owned",
    candidate_release_tag: null,
  });
  assert.deepEqual(validatePublicationAuthority({
    publicationMode: "preprovisioned-candidate",
    candidateReleaseTag: "component/lichess-bot/v1.0.19/windows-x86_64",
    expectedVersion: "1.0.19",
  }), {
    publication_mode: "preprovisioned-candidate",
    candidate_release_tag: "component/lichess-bot/v1.0.19/windows-x86_64",
  });
  assert.throws(() => validatePublicationAuthority({
    publicationMode: "preprovisioned-candidate",
    candidateReleaseTag: "",
    expectedVersion: "1.0.19",
  }), /requires a candidate release tag/u);
  assert.throws(() => validatePublicationAuthority({
    publicationMode: "workflow-owned",
    candidateReleaseTag: "component/lichess-bot/v1.0.19/windows-x86_64",
    expectedVersion: "1.0.19",
  }), /forbids a candidate release tag/u);
});

test("release preflight rejects publication authority that disagrees with workflow steps", () => {
  const workflowOwned = "gh release create component/provider/v1.2.3/windows-x86_64 artifact";
  const preprovisioned = [
    "gh release view $tag --json assets",
    "gh release upload $tag @assetPaths",
  ].join("\n");
  assert.equal(workflowPublicationAuthority(workflowOwned), "workflow-owned");
  assert.equal(workflowPublicationAuthority(preprovisioned), "preprovisioned-candidate");
  assert.throws(
    () => workflowPublicationAuthority("cosign attest-blob artifact"),
    /cannot be derived/u,
  );

  const fixtureRoot = fixture();
  try {
    const source = readFileSync(fixtureRoot.workflowPath, "utf8")
      .replace(
        "gh release create component/provider/v1.2.3/windows-x86_64 artifact",
        preprovisioned,
      );
    writeFileSync(fixtureRoot.workflowPath, source);
    const input = {
      repositoryRoot: fixtureRoot.root,
      expectedVersion: "1.2.3",
      expectedComponentId: "provider.component",
      workflowPath: fixtureRoot.workflowPath,
      requiredRunnerLabels: fixtureRoot.labels,
      releaseRef: "main",
      localHead: HEAD,
      remoteMainHead: HEAD,
      runnerWorkRoot: RUNNER_ROOT,
      repositorySlug: "owner/provider",
      forwardedWorkflowInputs: ["version", "target", "runner"],
    };
    assert.throws(
      () => validateReleaseSource({
        ...input,
        publicationMode: "workflow-owned",
      }),
      /does not match workflow requirement preprovisioned-candidate/u,
    );
    const accepted = validateReleaseSource({
      ...input,
      publicationMode: "preprovisioned-candidate",
      candidateReleaseTag: "component/provider/v1.2.3/windows-x86_64",
    });
    assert.equal(accepted.workflow_publication_mode, "preprovisioned-candidate");
  } finally {
    rmSync(fixtureRoot.root, { recursive: true, force: true });
  }
});

test("PowerShell runner preflight keeps filtered tool candidates array-shaped", () => {
  const source = readFileSync(
    new URL("./invoke-local-ephemeral-release.ps1", import.meta.url),
    "utf8",
  );
  assert.match(source, /\$vswhereCandidates\s*=\s*@\(@\([\s\S]*?Where-Object[\s\S]*?\}\)/u);
  assert.match(source, /\$vswhereCandidates\.Count\s*-lt\s*1/u);
  assert.match(source, /RunnerWorkRoot = 'C:\\ar'/u);
  assert.match(source, /Substring\(0, 16\)/u);
  assert.match(source, /--work 'w'/u);
  assert.match(source, /projected_path_characters = \$contract\.projected_path_characters/u);
  assert.match(source, /function Ensure-EmptyCandidateRelease/u);
  assert.match(source, /'api', '--method', 'POST', "repos\/\$Repository\/releases"/u);
  assert.match(source, /"tag_name=\$Tag"[\s\S]*?"target_commitish=\$Head"/u);
  assert.match(source, /'draft=false'[\s\S]*?'prerelease=true'[\s\S]*?'make_latest=false'/u);
  assert.match(source, /Candidate prerelease already contains assets/u);
  assert.match(source, /candidate_release_action/u);
  assert.match(source, /--publication-mode', \$PublicationMode/u);
  assert.match(source, /function Assert-PinnedReleaseInputPair/u);
  assert.match(source, /\$parsedUrl\.Query[\s\S]*?\$parsedUrl\.Fragment/u);
  assert.match(source, /\$Sha256 -notmatch '\^\[0-9a-f\]\{64\}\$'/u);
  assert.match(source, /onnxruntime_url[\s\S]*?onnxruntime_sha256/u);
  assert.match(source, /model_url[\s\S]*?model_sha256/u);
  assert.match(source, /\$ComponentId -eq 'uci_arena\.engine'[\s\S]*?@\('ONNX Runtime', 'Model'\)/u);
  assert.match(source, /uci_arena\.engine release requires an explicit \$requiredLabel URL and SHA-256/u);
  assert.match(source, /pinned_input_pairs = \$validatedReleaseInputPairs\.Count/u);
  assert.match(source, /forwarded_workflow_inputs = \$contract\.forwarded_workflow_inputs/u);
  assert.match(source, /\$forwardedWorkflowInputs = @\('version', 'target', 'runner'\)/u);
  assert.match(source, /if \(\$WorkflowComponent\) \{ \$forwardedWorkflowInputs \+= 'component' \}/u);
  assert.match(source, /@\('--workflow-input', \$inputName\)/u);
  assert.ok(source.indexOf("foreach ($pair in $validatedReleaseInputPairs)")
    < source.indexOf("$runId = Invoke-WorkflowDispatch"));
  assert.ok(source.indexOf("if ($ComponentId -eq 'uci_arena.engine')")
    < source.indexOf("$candidateReleaseState = Ensure-EmptyCandidateRelease"));
  assert.ok(source.indexOf("$candidateReleaseState = Ensure-EmptyCandidateRelease")
    < source.indexOf("actions/runners/registration-token"));
  assert.match(source, /function Invoke-GitHubJson[\s\S]*?attempt -le 3/u);
  assert.match(source, /\$registration = Invoke-GitHubJson[\s\S]*?actions\/runners\/registration-token/u);
  assert.match(source, /\$registrationToken = \[string\]\$registration\.token/u);
  assert.doesNotMatch(source, /registration-token" --jq \.token\)\.Trim\(\)/u);
  assert.match(source, /drainDeadline[\s\S]*?AddMinutes\(5\)/u);
  assert.match(source, /\$cleanupAuthorized\s*=\s*\$false/u);
  assert.match(source, /\[ValidateRange\(0, 4\)\]\[int\]\$RunnerCount = 0/u);
  assert.match(source, /--runner-count', \$RunnerCount/u);
  assert.match(source, /runner_label_sets = \$contract\.runner_label_sets/u);
  assert.match(source, /for \(\$runnerIndex = 1; \$runnerIndex -le \$contract\.runner_count/u);
  assert.match(source, /\$effectiveRunnerLabels = @\(\$contract\.runner_label_sets\[\$runnerIndex - 1\]\)/u);
  assert.match(source, /--labels \(\$effectiveRunnerLabels -join ','\)/u);
  assert.match(source, /\$runnerProcesses \+= \$runnerProcess/u);
  assert.match(source, /runner_count = \$contract\.runner_count/u);
  assert.match(source, /stdout_logs = \$stdoutLogs[\s\S]*?stderr_logs = \$stderrLogs/u);
  assert.match(source, /preserving ephemeral runner roots for the active workflow/u);
  assert.match(source, /\$priorRunIds\s*=\s*@\(\$priorRuns[\s\S]*?databaseId/u);
  assert.match(source, /-not \(\$PriorRunIds -contains \[string\]\$_\.databaseId\)/u);
  assert.doesNotMatch(source, /dispatchTime\.AddSeconds/u);
  assert.match(source, /function Invoke-WorkflowDispatch[\s\S]*?attempt -le 3[\s\S]*?observation -le 5[\s\S]*?Start-Sleep -Seconds 2/u);
  assert.match(source, /Invoke-WorkflowDispatch -Arguments \$dispatchArguments/u);
  assert.doesNotMatch(source, /& gh @dispatchArguments/u);
  assert.match(source, /\[Console\]::Error\.WriteLine\([\s\S]*?\$releaseStage/u);
  assert.match(source, /function Stop-RunnerProcessTree[\s\S]*?Get-CimInstance Win32_Process/u);
  assert.match(source, /Stop-RunnerProcessTree -RootProcessId \$rootProcessId[\s\S]*?-RunnerRoot \$runnerRoots/u);
  assert.match(source, /Runner process tree exceeds the cleanup bound/u);
  assert.match(source, /\} else \{[\s\S]*?'run', 'view'/u);
  assert.match(source, /foreach \(\$runnerRoot in \$runnerRoots\)[\s\S]*?Remove-Item/u);
  assert.match(source, /lastMonitorFailure[\s\S]*?Start-Sleep -Seconds 10/u);
});
