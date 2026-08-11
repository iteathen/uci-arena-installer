import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  validateReleaseIntegration,
  validateCandidateReleaseTag,
  validateRunnerPathBudget,
  validateReleaseSource,
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
    });
    assert.equal(result.version_sources, 3);
    assert.equal(result.runner_labels, 6);
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
      "    runs-on: '[\"self-hosted\",\"Windows\",\"X64\",\"ephemeral\",\"uci-suite\",\"manager-release\"]'",
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
    });
    assert.equal(result.version_sources, 1);
    assert.equal(result.component_id, "manager.core");
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
  assert.match(source, /gh release create \$Tag[\s\S]*?--target \$Head --prerelease/u);
  assert.match(source, /Candidate prerelease already contains assets/u);
  assert.match(source, /candidate_release_action/u);
  assert.ok(source.indexOf("$candidateReleaseState = Ensure-EmptyCandidateRelease")
    < source.indexOf("actions/runners/registration-token"));
  assert.match(source, /function Invoke-GitHubJson[\s\S]*?attempt -le 3/u);
  assert.match(source, /drainDeadline[\s\S]*?AddMinutes\(5\)/u);
  assert.match(source, /\$cleanupAuthorized\s*=\s*\$false/u);
  assert.match(source, /preserving ephemeral runner root for the active job/u);
  assert.match(source, /\$priorRunIds\s*=\s*@\(\$priorRuns[\s\S]*?databaseId/u);
  assert.match(source, /-not \(\$priorRunIds -contains \[string\]\$_\.databaseId\)/u);
  assert.doesNotMatch(source, /dispatchTime\.AddSeconds/u);
  assert.match(source, /\[void\]\$runnerProcess\.WaitForExit/u);
  assert.match(source, /elseif \(\$runnerProcess\.HasExited\)[\s\S]*?'run', 'view'/u);
  assert.match(source, /lastMonitorFailure[\s\S]*?Start-Sleep -Seconds 10/u);
});
