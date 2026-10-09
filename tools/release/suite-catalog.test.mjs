import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
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

test("catalog consumes normalized artifact facts without provider-specific release records", () => {
  // Logical consumer projection fixture only; this test establishes no signing
  // or release authority. The optional real-data integration exercises crypto.
  const candidates=composition.components.map(component=>{
    const entry=candidate(component);
    entry.verified={schema:'arena_verified_component_provenance_v1',component_id:component.component_id,
      artifact:{type:'archive',name:`${component.component_id}.tar.gz`,sha256:'c'.repeat(64),size_bytes:7}};
    delete entry.providerResult;delete entry.lockRow.artifact_name;delete entry.lockRow.artifact_sha256;
    return entry;
  });
  const catalog=buildSuiteCatalog({composition,target:'windows-x86_64',candidates,featureComponentIds:['example.book']});
  assert.deepEqual(catalog.components[1].artifacts['windows-x86_64'],{
    type:'archive',source:'payloads/example.book.tar.gz',sha256:'c'.repeat(64),size_bytes:7,
  });
});

test("current Windows feature policy composes the JS suite with optional root knowledge", () => {
  // Synthetic inventories test projection only; these are not release candidates.
  const selected = JSON.parse(readFileSync(new URL(
    "../../compositions/complete-arena-0.3.52.json", import.meta.url,
  ), "utf8"));
  const policy = JSON.parse(readFileSync(new URL(
    "../../packaging/windows/suite-bundle.json", import.meta.url,
  ), "utf8"));
  const candidates = selected.components.map((component) => {
    const entry = candidate(component);
    entry.manifest.dependencies = [...component.requires];
    return entry;
  });
  const catalog = buildSuiteCatalog({
    composition: selected, target: "windows-x86_64", candidates,
    featureComponentIds: policy.features.map((feature) => feature.component_id),
  });
  assert.equal(catalog.suite_version, "0.3.52");
  assert.deepEqual(catalog.feature_selection, {
    base_preset: "manager_only",
    components: ["uci_arena.vector", "book_forge.service", "syzygy.root-provider",
      "uci_arena_evidence.service", "lichess.adapter"],
    default_selected: ["uci_arena.vector", "book_forge.service", "syzygy.root-provider",
      "uci_arena_evidence.service", "lichess.adapter"],
  });
  const byId = new Map(catalog.components.map((entry) => [entry.id, entry]));
  assert.deepEqual(byId.get("uci_arena.vector").dependencies, ["node_runtime.private"]);
  assert.deepEqual(byId.get("book_forge.service").dependencies,
    ["node_runtime.private", "stockfish.engine"]);
  assert.deepEqual(byId.get("uci_arena_evidence.service").dependencies,
    ["node_runtime.private", "uci_arena.vector"]);
  assert.deepEqual(byId.get("lichess.adapter").dependencies, ["node_runtime.private"]);
  assert.deepEqual(byId.get("syzygy.root-provider").dependencies, []);
  assert.equal(byId.get("node_runtime.private").version, "26.11.1");
  assert.equal(byId.get("stockfish.engine").version, "15.1");
  assert.equal(selected.components.find((entry) => entry.component_id === "syzygy.root-provider").required, false);
  assert.equal(byId.get("syzygy.root-provider").version, "2.1.1");
  assert.deepEqual(catalog.optional_component_groups, { knowledge: [], books: [], tablebases: [] });

  // Clearing the optional provider stays a normal operator selection, independent of Vector.
  const withoutProvider = structuredClone(selected);
  withoutProvider.components.find((entry) => entry.component_id === "syzygy.root-provider").default_enabled = false;
  const optionalCatalog = buildSuiteCatalog({
    composition: withoutProvider, target: "windows-x86_64", candidates,
    featureComponentIds: policy.features.map((feature) => feature.component_id),
  });
  assert.deepEqual(optionalCatalog.feature_selection.default_selected,
    ["uci_arena.vector", "book_forge.service", "uci_arena_evidence.service", "lichess.adapter"]);
});
