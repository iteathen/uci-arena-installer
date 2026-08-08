import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

import { resolveComposition, validateProductComposition } from "./product-composition.mjs";

const root = path.resolve(import.meta.dirname, "../..");
const composition = JSON.parse(readFileSync(
  path.join(root, "compositions", "complete-arena-0.3.9.json"),
  "utf8",
));

test("complete composition declares independently versioned suite bricks", () => {
  const validated = validateProductComposition(composition);
  assert.equal(validated.product_release, "0.3.9");
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
      "manager.core": "0.3.9",
      "uci_arena.engine": "0.1.4",
      "uci_arena_evidence.runtime": "0.3.3",
      "lichess.adapter": "1.0.3",
      "book_forge.service": "0.4.0",
      "opening_book.standard": "0.4.0",
      "uci_arena_evidence.service": "0.3.10",
      "node_runtime.private": "24.15.0",
      "stockfish.engine": "15.1",
    },
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
