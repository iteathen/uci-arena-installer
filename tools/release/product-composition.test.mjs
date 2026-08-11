import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

import {
  resolveComposition,
  validateCompositionManifests,
  validateProductComposition,
} from "./product-composition.mjs";

const root = path.resolve(import.meta.dirname, "../..");
const composition = JSON.parse(readFileSync(
  path.join(root, "compositions", "complete-arena-0.3.18.json"),
  "utf8",
));

test("complete composition declares independently versioned suite bricks", () => {
  const validated = validateProductComposition(composition);
  assert.equal(validated.product_release, "0.3.18");
  assert.equal(validated.installer.repository, "iteathen/uci-arena-installer");
  assert.deepEqual(
    new Set(validated.components.map((component) => component.repository)),
    new Set([
      "iteathen/uci-arena-installer",
      "iteathen/uci-arena-manager",
      "iteathen/UCI_Arena",
      "iteathen/uci-arena-lichess-bot",
      "iteathen/uci-arena-book-forge",
      "iteathen/uci-arena-evidence-service",
    ]),
  );
  assert.deepEqual(
    Object.fromEntries(validated.components.map((component) => [
      component.component_id,
      component.version,
    ])),
    {
      "manager.core": "0.3.16",
      "uci_arena.engine": "0.1.9",
      "uci_arena_evidence.runtime": "0.3.4",
      "lichess.adapter": "1.0.9",
      "book_forge.service": "0.4.1",
      "opening_book.standard": "0.4.0",
      "uci_arena_evidence.service": "0.3.12",
      "node_runtime.private": "24.15.0",
      "stockfish.engine": "15.1",
    },
  );
  assert.deepEqual(
    validated.components.find(
      (component) => component.component_id === "uci_arena_evidence.runtime",
    ).requires,
    ["uci_arena.engine", "node_runtime.private"],
  );
});

test("composition resolution accepts substitution only through exact identities", () => {
  const available = composition.components.map((component) => ({
    component_id: component.component_id,
    version: component.version,
    repository: component.repository,
    artifact_sha256: "a".repeat(64),
  }));
  assert.equal(resolveComposition(composition, available).length, composition.components.length);
  const substituted = available.map((entry) => ({ ...entry }));
  substituted.find((entry) => entry.component_id === "uci_arena_evidence.service").repository =
    "untrusted/evidence-service";
  assert.throws(
    () => resolveComposition(composition, substituted),
    /exact component candidate is unavailable/u,
  );
});

test("composition rejects undeclared dependencies and dependency cycles", () => {
  const missing = structuredClone(composition);
  missing.components[0].requires.push("missing.component");
  assert.throws(() => validateProductComposition(missing), /requires undeclared/u);

  const cycle = structuredClone(composition);
  cycle.components.find((component) => component.component_id === "manager.core").requires =
    ["uci_arena.engine"];
  cycle.components.find((component) => component.component_id === "uci_arena.engine").requires =
    ["manager.core"];
  assert.throws(() => validateProductComposition(cycle), /dependency cycle/u);
});

function availableWithManifests(product = composition) {
  return product.components.map((component) => ({
    component_id: component.component_id,
    version: component.version,
    repository: component.repository,
    manifest: {
      schema: "arena_install_component_v1",
      schema_version: 1,
      component_id: component.component_id,
      component_version: component.version,
      dependencies: [...component.requires],
    },
  }));
}

test("resolved provider dependencies cannot be omitted by composition", () => {
  const available = availableWithManifests();
  assert.equal(validateCompositionManifests(composition, available).length, composition.components.length);

  const missing = structuredClone(composition);
  missing.components.find(
    (component) => component.component_id === "uci_arena_evidence.runtime",
  ).requires = ["uci_arena.engine"];
  assert.throws(
    () => validateCompositionManifests(missing, available),
    /composition omits manifest dependencies: node_runtime\.private/u,
  );
});

test("installer-integration workspace drift is rejected before catalog generation", () => {
  const available = availableWithManifests();
  const engine = available.find(
    (component) => component.component_id === "uci_arena.engine",
  ).manifest;
  engine.workspace_name = "UCI-Arena-Engine";
  engine.installer_integration = {
    schema: "arena_provider_installer_integration_v1",
    schema_version: 1,
    workspace_name: "UCI-Arena-Engine",
    workspace_placement: "product_data",
  };
  assert.equal(validateCompositionManifests(composition, available).length, composition.components.length);

  engine.installer_integration.workspace_placement = "program_files";
  assert.throws(
    () => validateCompositionManifests(composition, available),
    /installer-integration workspace is invalid or inconsistent for uci_arena\.engine/u,
  );
  engine.installer_integration.workspace_placement = "product_data";

  delete engine.workspace_name;
  assert.throws(
    () => validateCompositionManifests(composition, available),
    /installer-integration workspace is invalid or inconsistent for uci_arena\.engine/u,
  );

  engine.workspace_name = "different-workspace";
  assert.throws(
    () => validateCompositionManifests(composition, available),
    /installer-integration workspace is invalid or inconsistent for uci_arena\.engine/u,
  );
});

test("composition may add explicit suite wiring without rewriting provider manifests", () => {
  const expanded = structuredClone(composition);
  expanded.components.find(
    (component) => component.component_id === "opening_book.standard",
  ).requires = ["uci_arena.engine"];
  const available = availableWithManifests(expanded);
  available.find(
    (component) => component.component_id === "opening_book.standard",
  ).manifest.dependencies = [];
  assert.equal(validateCompositionManifests(expanded, available).length, expanded.components.length);
});

test("an omitted optional dependency projection means no manifest dependencies", () => {
  const available = availableWithManifests();
  const managedNode = available.find(
    (component) => component.component_id === "node_runtime.private",
  );
  delete managedNode.manifest.dependencies;
  assert.equal(validateCompositionManifests(composition, available).length, composition.components.length);

  managedNode.manifest.dependencies = null;
  assert.throws(
    () => validateCompositionManifests(composition, available),
    /resolved manifest dependencies are invalid/u,
  );
});
