import path from "node:path";

const SCOPES = ["per-user", "per-machine"];
const ROOTS = [
  "integration", "data", "install", "cache", "workspace", "knowledge",
  "release", "receipts", "transactions", "preservation",
];
const STABLE_PATHS = [
  "policy", "descriptor", "scope_marker", "setup_launcher",
  "installer_runtime", "path_directory",
];
const REGISTRATION_VALUES = [
  "schema", "scope", "installation_id", "layout_generation",
  "integration_root", "policy_path", "policy_sha256", "descriptor_path",
  "descriptor_sha256", "setup_launcher_path",
];
const ANCHORS = {
  user_local_app_data: { scope: "per-user", wix: "LocalAppDataFolder" },
  machine_program_files: { scope: "per-machine", wix: "ProgramFiles64Folder" },
  machine_program_data: { scope: "per-machine", wix: "CommonAppDataFolder" },
};
const RESERVED = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$/iu;
const IDENTIFIER = /^[A-Za-z0-9._-]+$/u;
const REGISTRY_VALUE = /^[A-Za-z][A-Za-z0-9_]*$/u;

function fail(field, reason) {
  throw new TypeError(`invalid install layout ${String(field).slice(0, 160)}: ${reason}`);
}

function object(value, field) {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    fail(field, "expected object");
  }
  return value;
}

function exactKeys(value, expected, field) {
  const keys = Object.keys(object(value, field));
  const wanted = new Set(expected);
  for (const key of keys) {
    if (!wanted.has(key)) fail(`${field}.${key}`, "unknown field");
  }
  for (const key of expected) {
    if (!Object.hasOwn(value, key)) fail(`${field}.${key}`, "missing field");
  }
}

function text(value, field, maximum) {
  if (typeof value !== "string" || value.length === 0 || value.length > maximum) {
    fail(field, "expected bounded nonempty string");
  }
  if (/\p{Cc}/u.test(value)) fail(field, "control character");
  return value;
}

function segment(value, field) {
  text(value, field, 128);
  if (value === "." || value === ".." || /[<>:"/\\|?*]/u.test(value)) {
    fail(field, "unsafe Windows segment");
  }
  if (/[. ]$/u.test(value) || RESERVED.test(value)) {
    fail(field, "reserved Windows segment");
  }
  return value;
}

function segments(value, field) {
  if (!Array.isArray(value) || value.length > 16) fail(field, "invalid segments");
  return value.map((item, index) => segment(item, `${field}[${index}]`));
}

function relativePath(value, field) {
  text(value, field, 512);
  if (value.includes("\\") || value.startsWith("/") || /^[A-Za-z]:/u.test(value)
      || value.includes("//")) {
    fail(field, "invalid relative path");
  }
  const parts = value.split("/");
  if (parts.some((part) => part === "" || part === "." || part === "..")) {
    fail(field, "invalid relative path component");
  }
  parts.forEach((part, index) => segment(part, `${field}[${index}]`));
  return value;
}

function registrySubkey(value) {
  text(value, "registration.subkey", 256);
  if (value.startsWith("\\") || value.includes("\\\\")) {
    fail("registration.subkey", "invalid registry subkey");
  }
  value.split("\\").forEach((part, index) => {
    if (!/^[A-Za-z0-9 ._-]+$/u.test(part) || /[. ]$/u.test(part)) {
      fail(`registration.subkey[${index}]`, "invalid registry segment");
    }
  });
  return value;
}

function caseKey(value) {
  return value.toLocaleLowerCase("en-US");
}

function uniqueCaseInsensitive(values, field) {
  const seen = new Set();
  for (const value of values) {
    const key = caseKey(value.replaceAll("/", "\\"));
    if (seen.has(key)) fail(field, "case-insensitive collision");
    seen.add(key);
  }
}

function validateRoot(value, field, anchored) {
  exactKeys(value, anchored ? ["anchor", "segments"] : ["relative_to", "segments"], field);
  if (anchored) {
    if (!Object.hasOwn(ANCHORS, value.anchor)) fail(`${field}.anchor`, "unknown anchor");
  } else if (value.relative_to !== "integration" && value.relative_to !== "data") {
    fail(`${field}.relative_to`, "unknown root owner");
  }
  segments(value.segments, `${field}.segments`);
}

function freeze(value) {
  if (value && typeof value === "object" && !Object.isFrozen(value)) {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}

function isDescendant(parent, candidate) {
  const relative = path.win32.relative(parent, candidate);
  return relative !== "" && relative !== ".." && !relative.startsWith(`..${path.win32.sep}`)
    && !path.win32.isAbsolute(relative);
}

function validateAnchor(value, field) {
  text(value, field, 32768);
  if (value.includes("/") || value.startsWith("\\\\?\\") || value.startsWith("\\\\.\\")
      || /[. ]$/u.test(value) || path.win32.normalize(value) !== value
      || !path.win32.isAbsolute(value)) {
    fail(field, "anchor must be an absolute normalized Windows path");
  }
  const parsed = path.win32.parse(value);
  if (caseKey(value) === caseKey(parsed.root)) fail(field, "anchor may not be a root");
  return value;
}

function joinDescendant(parent, parts, field) {
  const result = path.win32.join(parent, ...parts);
  if (!isDescendant(parent, result)) fail(field, "path escapes parent");
  return result;
}

export function validateInstallLayoutPolicy(candidate) {
  exactKeys(candidate, [
    "schema", "schema_version", "policy_id", "policy_version", "platform",
    "registration", "stable_paths", "scopes",
  ], "policy");
  if (candidate.schema !== "arena_install_layout_policy_v1") fail("schema", "unsupported value");
  if (candidate.schema_version !== 1) fail("schema_version", "unsupported value");
  if (typeof candidate.policy_id !== "string" || candidate.policy_id.length > 128
      || !IDENTIFIER.test(candidate.policy_id)) fail("policy_id", "invalid identifier");
  if (!Number.isSafeInteger(candidate.policy_version) || candidate.policy_version < 1) {
    fail("policy_version", "invalid version");
  }
  if (candidate.platform !== "windows-x86_64") fail("platform", "unsupported value");

  exactKeys(candidate.registration, ["subkey", "values"], "registration");
  registrySubkey(candidate.registration.subkey);
  exactKeys(candidate.registration.values, REGISTRATION_VALUES, "registration.values");
  for (const name of REGISTRATION_VALUES) {
    const value = candidate.registration.values[name];
    if (typeof value !== "string" || value.length > 128 || !REGISTRY_VALUE.test(value)) {
      fail(`registration.values.${name}`, "invalid value name");
    }
  }
  uniqueCaseInsensitive(Object.values(candidate.registration.values), "registration.values");

  exactKeys(candidate.stable_paths, STABLE_PATHS, "stable_paths");
  for (const name of STABLE_PATHS) relativePath(candidate.stable_paths[name], `stable_paths.${name}`);
  uniqueCaseInsensitive(Object.values(candidate.stable_paths), "stable_paths");
  const expectedPolicy = `share/uci-arena/layout-policies/${candidate.policy_id}-v${candidate.policy_version}.json`;
  if (candidate.stable_paths.policy !== expectedPolicy) fail("stable_paths.policy", "identity mismatch");

  exactKeys(candidate.scopes, SCOPES, "scopes");
  for (const scope of SCOPES) {
    const definition = candidate.scopes[scope];
    exactKeys(definition, ["privilege", "roots"], `scopes.${scope}`);
    const expectedPrivilege = scope === "per-user" ? "user" : "machine";
    if (definition.privilege !== expectedPrivilege) fail(`scopes.${scope}.privilege`, "scope mismatch");
    exactKeys(definition.roots, ROOTS, `scopes.${scope}.roots`);
    for (const root of ROOTS) validateRoot(definition.roots[root], `scopes.${scope}.roots.${root}`, root === "integration" || root === "data");
    for (const root of ["integration", "data"]) {
      if (ANCHORS[definition.roots[root].anchor].scope !== scope) {
        fail(`scopes.${scope}.roots.${root}.anchor`, "scope mismatch");
      }
    }
    if (candidate.policy_version === 2) {
      const install = definition.roots.install;
      if (install.relative_to !== "integration" || install.segments.length !== 1
          || install.segments[0] !== "installed") fail(`scopes.${scope}.roots.install`, "v2 invariant");
      for (const root of ROOTS.slice(3)) {
        const item = definition.roots[root];
        if (item.relative_to !== "data" || item.segments.length !== 1) {
          fail(`scopes.${scope}.roots.${root}`, "v2 mutable-root invariant");
        }
      }
      uniqueCaseInsensitive(ROOTS.slice(3).map((root) => definition.roots[root].segments[0]), `scopes.${scope}.roots`);
    }
  }
  return freeze(structuredClone(candidate));
}

export function resolveInstallLayout({ policy, scope, anchors }) {
  const validated = validateInstallLayoutPolicy(policy);
  if (!SCOPES.includes(scope)) fail("scope", "unsupported value");
  object(anchors, "anchors");
  const definition = validated.scopes[scope];
  const needed = new Set([definition.roots.integration.anchor, definition.roots.data.anchor]);
  const resolvedAnchors = {};
  for (const anchor of needed) resolvedAnchors[anchor] = validateAnchor(anchors[anchor], `anchors.${anchor}`);
  const integration = joinDescendant(resolvedAnchors[definition.roots.integration.anchor], definition.roots.integration.segments, "roots.integration");
  const data = joinDescendant(resolvedAnchors[definition.roots.data.anchor], definition.roots.data.segments, "roots.data");
  if (caseKey(integration) === caseKey(data) || isDescendant(integration, data) || isDescendant(data, integration)) {
    fail("roots", "integration and data overlap");
  }
  const roots = { data, integration };
  for (const name of ROOTS.slice(2)) {
    const item = definition.roots[name];
    roots[name] = joinDescendant(roots[item.relative_to], item.segments, `roots.${name}`);
  }
  const orderedRoots = Object.fromEntries(ROOTS.slice(1).map((name) => [name, roots[name]]));
  orderedRoots.data = data;
  const stable = Object.fromEntries(STABLE_PATHS.map((name) => [
    name,
    joinDescendant(integration, validated.stable_paths[name].split("/"), `stable_paths.${name}`),
  ]));
  uniqueCaseInsensitive([...Object.values(roots), ...Object.values(stable)], "resolved paths");
  return freeze({
    schema: "arena_resolved_install_layout_v1",
    policy_id: validated.policy_id,
    policy_version: validated.policy_version,
    platform: validated.platform,
    scope,
    privilege: definition.privilege,
    integration_root: integration,
    roots: orderedRoots,
    stable_paths: stable,
    registration: {
      hive: scope === "per-user" ? "HKCU" : "HKLM",
      view: "64",
      subkey: validated.registration.subkey,
      values: structuredClone(validated.registration.values),
    },
  });
}

function wixExpression(folder, parts) {
  return `[${folder}]${parts.join("\\")}`;
}

export function buildWindowsWixLayoutProjection({ policy, scope }) {
  const validated = validateInstallLayoutPolicy(policy);
  if (!SCOPES.includes(scope)) fail("scope", "unsupported value");
  const definition = validated.scopes[scope];
  const anchorProjection = (root) => {
    const item = definition.roots[root];
    const wix = ANCHORS[item.anchor].wix;
    return {
      semantic_anchor: item.anchor,
      wix_folder_id: wix,
      segments: [...item.segments],
      expression: wixExpression(wix, item.segments),
    };
  };
  const roots = {};
  for (const name of ROOTS.slice(2)) {
    const item = definition.roots[name];
    roots[name] = {
      relative_to: item.relative_to,
      segments: [...item.segments],
      expression: item.segments.join("\\"),
    };
  }
  return freeze({
    schema: "arena_windows_wix_layout_projection_v1",
    policy_id: validated.policy_id,
    policy_version: validated.policy_version,
    scope,
    privilege: definition.privilege,
    anchors: { integration: anchorProjection("integration"), data: anchorProjection("data") },
    roots,
    stable_paths: structuredClone(validated.stable_paths),
    registration: {
      hive: scope === "per-user" ? "HKCU" : "HKLM",
      view: "64",
      subkey: validated.registration.subkey,
      values: structuredClone(validated.registration.values),
    },
  });
}
