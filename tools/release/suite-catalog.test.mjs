import assert from "node:assert/strict";
import test from "node:test";

import { buildSuiteCatalog } from "./suite-catalog.mjs";

const composition = {
  schema: "arena_product_composition_v1",
  product_release: "1.2.3",
  installer: {
    repository: "example/installer",
    minimum_version: "1.0.0",
  },
  components: [
    {
      role: "manager",
      component_id: "manager.core",
      version: "1.0.0",
      repository: "example/manager",
      required: true,
      default_enabled: true,
      requires: [],
    },
    {
      role: "knowledge",
      component_id: "example.book",
      version: "2.0.0",
      repository: "example/book-provider",
      required: false,
      default_enabled: true,
      requires: [],
    },
  ],
};

function candidate(component, { activation = undefined } = {}) {
  const manifest = {
    schema: "arena_install_component_v1",
    schema_version: 1,
    component_id: component.component_id,
    component_version: component.version,
    target_triple: "windows-x86_64",
    entrypoints: { cli: "bin/tool.exe" },
    dependencies: [],
    files: [{ path: "bin/tool.exe", sha256: "a".repeat(64), size_bytes: 10 }],
  };
  if (activation) manifest.activation = activation;
  return {
    repository: component.repository,
    lockRow: {
      component_id: component.component_id,
      component_version: component.version,
      target_triple: "windows-x86_64",
      artifact_name: `${component.component_id}.tar.gz`,
      artifact_sha256: "b".repeat(64),
    },
    providerResult: { artifact_type: "archive" },
    artifactSizeBytes: 7,
    manifestSizeBytes: 5,
    manifest,
  };
}

test("catalog derives dependencies, features, capacities, and knowledge groups from contracts", () => {
  const candidates = [
    candidate(composition.components[0]),
    candidate(composition.components[1], {
      activation: [{
        kind: "opening_book",
        source_path: "book.bin",
        destination_root: "knowledge_root",
        destination_path: "books/book.bin",
      }],
    }),
  ];
  const catalog = buildSuiteCatalog({
    composition,
    target: "windows-x86_64",
    candidates,
    featureComponentIds: ["example.book"],
  });
  assert.deepEqual(catalog.presets.manager_only, ["manager.core"]);
  assert.deepEqual(catalog.feature_selection.default_selected, ["example.book"]);
  assert.deepEqual(catalog.optional_component_groups.books, ["example.book"]);
  assert.equal(catalog.components[1].activation_role, "knowledge");
  assert.equal(catalog.components[1].installed_bytes, 15);
  assert.equal(
    catalog.components[1].artifacts["windows-x86_64"].source,
    "payloads/example.book.tar.gz",
  );
});

test("catalog refuses composition that omits a provider manifest dependency", () => {
  const candidates = [
    candidate(composition.components[0]),
    candidate(composition.components[1]),
  ];
  candidates[1].manifest.dependencies = ["manager.core"];
  assert.throws(() => buildSuiteCatalog({
    composition,
    target: "windows-x86_64",
    candidates,
    featureComponentIds: ["example.book"],
  }), /composition omits manifest dependencies/u);
});
