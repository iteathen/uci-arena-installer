#!/usr/bin/env node

import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";

const SEMVER = /^[0-9]+\.[0-9]+\.[0-9]+$/u;
const COMPONENT_VERSION = /^[0-9]+\.[0-9]+(?:\.[0-9]+)?$/u;
const REPOSITORY = /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u;
const ROLE = /^[a-z][a-z0-9_]*$/u;
const COMPONENT_ID = /^[a-z0-9][a-z0-9._-]*$/u;

function fail(message) {
  throw new Error(`product composition: ${message}`);
}

function exactKeys(value, expected, context) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail(`${context} must be an object`);
  }
  const actual = Object.keys(value).sort();
  const wanted = [...expected].sort();
  if (actual.length !== wanted.length || actual.some((key, index) => key !== wanted[index])) {
    fail(`${context} fields must be exactly ${wanted.join(", ")}`);
  }
}

export function validateProductComposition(candidate) {
  exactKeys(candidate, ["schema", "product_release", "installer", "components"], "root");
  if (candidate.schema !== "arena_product_composition_v1") fail("schema is unsupported");
  if (!SEMVER.test(candidate.product_release)) fail("product_release must be semantic version text");
  exactKeys(candidate.installer, ["repository", "minimum_version"], "installer");
  if (!REPOSITORY.test(candidate.installer.repository)) fail("installer repository is invalid");
  if (!SEMVER.test(candidate.installer.minimum_version)) fail("installer minimum_version is invalid");
  if (!Array.isArray(candidate.components) || candidate.components.length === 0) {
    fail("components must be a nonempty array");
  }

  const roles = new Set();
  const ids = new Set();
  for (const [index, component] of candidate.components.entries()) {
    const context = `components[${index}]`;
    exactKeys(component, [
      "role", "component_id", "version", "repository", "required", "default_enabled", "requires",
    ], context);
    if (!ROLE.test(component.role) || roles.has(component.role)) fail(`${context} role is invalid or duplicated`);
    if (!COMPONENT_ID.test(component.component_id) || ids.has(component.component_id)) {
      fail(`${context} component_id is invalid or duplicated`);
    }
    if (!COMPONENT_VERSION.test(component.version)) fail(`${context} version is invalid`);
    if (!REPOSITORY.test(component.repository)) fail(`${context} repository is invalid`);
    if (typeof component.required !== "boolean" || typeof component.default_enabled !== "boolean") {
      fail(`${context} selection fields must be boolean`);
    }
    if (component.required && !component.default_enabled) {
      fail(`${context} required component cannot be disabled by default`);
    }
    if (!Array.isArray(component.requires) || new Set(component.requires).size !== component.requires.length
        || component.requires.some((dependency) => !COMPONENT_ID.test(dependency))) {
      fail(`${context} requires must contain unique component ids`);
    }
    roles.add(component.role);
    ids.add(component.component_id);
  }

  for (const component of candidate.components) {
    for (const dependency of component.requires) {
      if (!ids.has(dependency)) fail(`${component.component_id} requires undeclared ${dependency}`);
      if (dependency === component.component_id) fail(`${component.component_id} cannot depend on itself`);
    }
  }

  const visiting = new Set();
  const visited = new Set();
  const byId = new Map(candidate.components.map((component) => [component.component_id, component]));
  const visit = (id) => {
    if (visiting.has(id)) fail(`dependency cycle includes ${id}`);
    if (visited.has(id)) return;
    visiting.add(id);
    for (const dependency of byId.get(id).requires) visit(dependency);
    visiting.delete(id);
    visited.add(id);
  };
  for (const id of ids) visit(id);

  return structuredClone(candidate);
}

export function resolveComposition(candidate, availableComponents) {
  const composition = validateProductComposition(candidate);
  if (!Array.isArray(availableComponents)) fail("available components must be an array");
  const available = new Map();
  for (const entry of availableComponents) {
    if (!entry || typeof entry !== "object" || !COMPONENT_ID.test(entry.component_id ?? "")
        || !COMPONENT_VERSION.test(entry.version ?? "") || !REPOSITORY.test(entry.repository ?? "")) {
      fail("available component identity is invalid");
    }
    const key = `${entry.component_id}\u0000${entry.version}\u0000${entry.repository}`;
    if (available.has(key)) fail(`available component identity is duplicated: ${entry.component_id}`);
    available.set(key, entry);
  }
  return composition.components.map((component) => {
    const key = `${component.component_id}\u0000${component.version}\u0000${component.repository}`;
    const resolved = available.get(key);
    if (!resolved) fail(`exact component candidate is unavailable: ${component.component_id}@${component.version}`);
    return { ...component, candidate: resolved };
  });
}

export function validateCompositionManifests(candidate, availableComponents) {
  const resolved = resolveComposition(candidate, availableComponents);
  for (const component of resolved) {
    const manifest = component.candidate?.manifest;
    if (!manifest || typeof manifest !== "object" || Array.isArray(manifest)
        || manifest.schema !== "arena_install_component_v1"
        || manifest.schema_version !== 1
        || manifest.component_id !== component.component_id
        || manifest.component_version !== component.version) {
      fail(`resolved manifest identity differs for ${component.component_id}`);
    }
    if (!Array.isArray(manifest.dependencies)
        || new Set(manifest.dependencies).size !== manifest.dependencies.length
        || manifest.dependencies.some((dependency) => !COMPONENT_ID.test(dependency))) {
      fail(`resolved manifest dependencies are invalid for ${component.component_id}`);
    }
    const compositionDependencies = new Set(component.requires);
    const omitted = manifest.dependencies.filter(
      (dependency) => !compositionDependencies.has(dependency),
    );
    if (omitted.length > 0) {
      fail(
        `${component.component_id} composition omits manifest dependencies: ${omitted.join(", ")}`,
      );
    }
  }
  return resolved;
}

function main(argv) {
  if (argv.length !== 1) fail("usage: product-composition.mjs <composition.json>");
  const filename = path.resolve(argv[0]);
  const composition = validateProductComposition(JSON.parse(readFileSync(filename, "utf8")));
  process.stdout.write(`${composition.product_release}: ${composition.components.length} components valid\n`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}
