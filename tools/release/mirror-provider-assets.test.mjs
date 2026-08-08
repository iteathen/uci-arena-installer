import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

import { buildProviderMirrorPlan } from "./mirror-provider-assets.mjs";

const root = path.resolve(import.meta.dirname, "../..");
const checkedInComposition = JSON.parse(readFileSync(
  path.join(root, "compositions", "complete-arena-0.3.9.json"),
  "utf8",
));
const checkedInDraft = JSON.parse(readFileSync(
  path.join(root, "release", "catalog.lock.json"),
  "utf8",
));
const digest = "a".repeat(64);

function fixture() {
  const composition = {
    schema: "arena_product_composition_v1",
    product_release: "1.2.3",
    installer: {
      repository: "iteathen/uci-arena-installer",
      minimum_version: "1.0.0",
    },
    components: [{
      role: "example",
      component_id: "example.service",
      version: "4.5.6",
      repository: "example/provider",
      required: true,
      default_enabled: true,
      requires: [],
    }],
  };
  const provider = {
    repository: "example/provider",
    workflow_path: ".github/workflows/component-release.yml",
    workflow_name: "Example component candidate",
  };
  const row = (target) => {
    const artifact = `example.service-4.5.6-${target}.tar.gz`;
    return {
      provider_id: "example",
      component_id: "example.service",
      component_version: "4.5.6",
      target_triple: target,
      source_commit: "b".repeat(40),
      provider_release_tag: `component/example.service/v4.5.6/${target}`,
      artifact_name: artifact,
      artifact_sha256: digest,
      dependencies: [],
      provider_result_name: `example.service-4.5.6-${target}.provider-result.json`,
      provider_result_sha256: digest,
      provenance_bundle_name: `${artifact}.sigstore.json`,
      provenance_bundle_sha256: digest,
      provenance_certificate_identity:
        "https://github.com/example/provider/.github/workflows/component-release.yml@refs/heads/main",
      provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
      provenance_workflow_name: provider.workflow_name,
      provenance_workflow_ref: "refs/heads/main",
      provenance_workflow_trigger: "workflow_dispatch",
    };
  };
  return {
    composition,
    lock: {
      schema: "arena_suite_release_lock_v2",
      suite_version: "1.2.3",
      release_ready: true,
      composition: { manifest_name: "example.json", sha256: digest },
      providers: { example: provider },
      provider_asset_mirror: {
        repository: "iteathen/uci-arena-installer",
        release_tag: "provider-mirror/v1.2.3",
      },
      managed_repository: "iteathen/uci-arena-installer",
      components: [row("windows-x86_64"), row("linux-x86_64")],
      managed_components: [],
    },
  };
}

test("checked-in draft supports read-only mirror inspection only", () => {
  const plan = buildProviderMirrorPlan(checkedInDraft, checkedInComposition, {
    allowDraft: true,
  });
  assert.equal(plan.repository, "iteathen/uci-arena-installer");
  assert.equal(plan.assets.length, 0);
  assert.throws(
    () => buildProviderMirrorPlan(checkedInDraft, checkedInComposition),
    /not release-ready/u,
  );
});

test("mirror plan retains original provider identity and exact asset digests", () => {
  const { lock, composition } = fixture();
  const plan = buildProviderMirrorPlan(lock, composition);
  assert.equal(plan.assets.length, 6);
  assert.ok(plan.assets.every((asset) => asset.sourceRepository === "example/provider"));
  assert.ok(plan.assets.every((asset) => asset.sha256 === digest));
  assert.deepEqual(
    [...plan.assets].map((asset) => asset.name),
    [...plan.assets].map((asset) => asset.name).sort(),
  );
});

test("mirror plan rejects globally ambiguous asset names", () => {
  const { lock, composition } = fixture();
  lock.components[1].artifact_name = lock.components[0].artifact_name;
  assert.throws(
    () => buildProviderMirrorPlan(lock, composition),
    /mirror asset name is duplicated/u,
  );
});
