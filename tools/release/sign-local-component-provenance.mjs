#!/usr/bin/env node

import { createHash, randomBytes } from "node:crypto";
import { createReadStream } from "node:fs";
import {
  chmod,
  lstat,
  mkdtemp,
  readFile,
  readdir,
  realpath,
  rename,
  rm,
  unlink,
  writeFile,
} from "node:fs/promises";
import { isDeepStrictEqual } from "node:util";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import {
  buildComponentProvenanceStatement,
  compareVerifiedProvenance,
  validateComponentReleaseEvidence,
} from "./component-provenance.mjs";
import { resolveCosignExecutable, runCosign } from "./cosign-cli.mjs";
import { verifyLocalKeyedCandidate } from "./local-provenance.mjs";
import {
  admitTrustedPublicKey,
  producerById,
  requireProducerScope,
  validateProvenanceTrustPolicy,
} from "./provenance-trust-policy.mjs";

const JSON_BYTES_MAX = 16 * 1024 * 1024;
const ARTIFACT_BYTES_MAX = 1024n * 1024n * 1024n * 1024n;
const PATH_BYTES_MAX = 32 * 1024;
const COSIGN_TIMEOUT_MS = 180_000;
const COSIGN_OUTPUT_BYTES_MAX = 1024 * 1024;
const SAFE_LEAF = /^[A-Za-z0-9._-]{1,255}$/u;
const FLAGS = new Set([
  "--artifact",
  "--evidence",
  "--trust-policy",
  "--producer-id",
  "--signing-key",
  "--output-directory",
  "--cosign",
]);
const REQUIRED_FLAGS = [
  "--artifact",
  "--evidence",
  "--trust-policy",
  "--producer-id",
  "--signing-key",
  "--output-directory",
];
const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

function fail(message) {
  throw new Error(`local provenance signer: ${message}`);
}

function safeLeaf(value, label) {
  if (typeof value !== "string"
      || !SAFE_LEAF.test(value)
      || path.basename(value) !== value
      || value === "."
      || value === "..") {
    fail(`${label} is invalid`);
  }
  return value;
}

function normalizedAbsolutePath(value, label) {
  if (typeof value !== "string"
      || !path.isAbsolute(value)
      || path.normalize(value) !== value
      || path.parse(value).root === value
      || Buffer.byteLength(value, "utf8") > PATH_BYTES_MAX
      || /[\u0000-\u001f\u007f]/u.test(value)) {
    fail(`${label} is invalid`);
  }
  return value;
}

function samePath(left, right) {
  return process.platform === "win32"
    ? path.resolve(left).toLowerCase() === path.resolve(right).toLowerCase()
    : path.resolve(left) === path.resolve(right);
}

function atOrBeneath(candidate, root) {
  const relative = path.relative(root, candidate);
  return relative === "" || (!relative.startsWith("..") && !path.isAbsolute(relative));
}

function sameFileIdentity(before, after) {
  return before.dev === after.dev
    && before.ino === after.ino
    && before.size === after.size
    && before.mtimeNs === after.mtimeNs;
}

function sameObjectIdentity(before, after) {
  return before.dev === after.dev && before.ino === after.ino;
}

async function admitRegularFile(filename, label, maximumBytes) {
  const admittedPath = normalizedAbsolutePath(filename, label);
  let before;
  try {
    before = await lstat(admittedPath, { bigint: true });
  } catch {
    fail(`${label} is unavailable`);
  }
  if (!before.isFile()
      || before.isSymbolicLink()
      || before.size < 1n
      || before.size > maximumBytes) {
    fail(`${label} must be an exact nonempty regular file`);
  }
  let canonical;
  try {
    canonical = await realpath(admittedPath);
  } catch {
    fail(`${label} identity is unavailable`);
  }
  if (!samePath(canonical, admittedPath)) fail(`${label} must not traverse a link`);
  return Object.freeze({ path: admittedPath, status: before });
}

async function readAdmittedFile(admitted, label) {
  let bytes;
  try {
    bytes = await readFile(admitted.path);
  } catch {
    fail(`${label} read failed`);
  }
  let after;
  try {
    after = await lstat(admitted.path, { bigint: true });
  } catch {
    fail(`${label} changed during admission`);
  }
  if (!sameFileIdentity(admitted.status, after)
      || BigInt(bytes.byteLength) !== admitted.status.size) {
    fail(`${label} changed during admission`);
  }
  return Buffer.from(bytes);
}

async function requireStableAdmittedFile(admitted, label) {
  let status;
  let canonical;
  try {
    [status, canonical] = await Promise.all([
      lstat(admitted.path, { bigint: true }),
      realpath(admitted.path),
    ]);
  } catch {
    fail(`${label} changed during use`);
  }
  if (!status.isFile()
      || status.isSymbolicLink()
      || !sameFileIdentity(admitted.status, status)
      || !samePath(admitted.path, canonical)) {
    fail(`${label} changed during use`);
  }
}

async function digestArtifact(admitted) {
  const hash = createHash("sha256");
  let size = 0n;
  try {
    for await (const chunk of createReadStream(admitted.path)) {
      hash.update(chunk);
      size += BigInt(chunk.length);
    }
  } catch {
    fail("artifact read failed");
  }
  let after;
  try {
    after = await lstat(admitted.path, { bigint: true });
  } catch {
    fail("artifact changed during admission");
  }
  if (!sameFileIdentity(admitted.status, after) || size !== admitted.status.size) {
    fail("artifact changed during admission");
  }
  return Object.freeze({ size, sha256: hash.digest("hex") });
}

function parseUniqueJson(bytes, label) {
  let source;
  try {
    source = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    fail(`${label} is not valid UTF-8 JSON`);
  }
  let index = 0;
  let depth = 0;
  const whitespace = () => {
    while (/[\t\n\r ]/u.test(source[index] ?? "")) index += 1;
  };
  const parseString = () => {
    if (source[index] !== "\"") fail(`${label} JSON is invalid`);
    const start = index;
    index += 1;
    while (index < source.length) {
      const character = source[index];
      index += 1;
      if (character === "\"") {
        try {
          return JSON.parse(source.slice(start, index));
        } catch {
          fail(`${label} JSON is invalid`);
        }
      }
      if (character === "\\") index += 1;
      else if (character < " ") fail(`${label} JSON is invalid`);
    }
    fail(`${label} JSON is invalid`);
  };
  const parseValue = () => {
    whitespace();
    depth += 1;
    if (depth > 128) fail(`${label} JSON nesting is invalid`);
    let result;
    const character = source[index];
    if (character === "{") result = parseObject();
    else if (character === "[") result = parseArray();
    else if (character === "\"") result = parseString();
    else if (source.startsWith("true", index)) {
      index += 4;
      result = true;
    } else if (source.startsWith("false", index)) {
      index += 5;
      result = false;
    } else if (source.startsWith("null", index)) {
      index += 4;
      result = null;
    } else {
      const match = source.slice(index).match(
        /^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/u,
      );
      if (!match) fail(`${label} JSON is invalid`);
      index += match[0].length;
      result = Number(match[0]);
      if (!Number.isFinite(result)) fail(`${label} JSON number is invalid`);
    }
    depth -= 1;
    return result;
  };
  const parseObject = () => {
    index += 1;
    whitespace();
    const result = {};
    const keys = new Set();
    if (source[index] === "}") {
      index += 1;
      return result;
    }
    for (;;) {
      whitespace();
      const key = parseString();
      if (keys.has(key)) fail(`${label} JSON has a duplicate key`);
      keys.add(key);
      whitespace();
      if (source[index] !== ":") fail(`${label} JSON is invalid`);
      index += 1;
      Object.defineProperty(result, key, {
        value: parseValue(),
        enumerable: true,
        configurable: true,
        writable: true,
      });
      whitespace();
      const separator = source[index];
      index += 1;
      if (separator === "}") return result;
      if (separator !== ",") fail(`${label} JSON is invalid`);
    }
  };
  const parseArray = () => {
    index += 1;
    whitespace();
    const result = [];
    if (source[index] === "]") {
      index += 1;
      return result;
    }
    for (;;) {
      result.push(parseValue());
      whitespace();
      const separator = source[index];
      index += 1;
      if (separator === "]") return result;
      if (separator !== ",") fail(`${label} JSON is invalid`);
    }
  };
  const result = parseValue();
  whitespace();
  if (index !== source.length) fail(`${label} JSON is invalid`);
  return result;
}

async function admitOutputDirectory(outputDirectory) {
  const outputPath = normalizedAbsolutePath(outputDirectory, "output directory");
  safeLeaf(path.basename(outputPath), "output directory leaf");
  const parent = path.dirname(outputPath);
  let status;
  try {
    status = await lstat(parent, { bigint: true });
  } catch {
    fail("output directory parent is unavailable");
  }
  if (!status.isDirectory() || status.isSymbolicLink()) {
    fail("output directory parent is invalid");
  }
  let canonical;
  try {
    canonical = await realpath(parent);
  } catch {
    fail("output directory parent identity is unavailable");
  }
  if (!samePath(parent, canonical)) fail("output directory parent must not traverse a link");
  await requireAbsentOutput(outputPath);
  return Object.freeze({
    path: outputPath,
    parent,
    leaf: path.basename(outputPath),
    parentStatus: status,
  });
}

async function requireStableOutputParent(output) {
  let status;
  let canonical;
  try {
    [status, canonical] = await Promise.all([
      lstat(output.parent, { bigint: true }),
      realpath(output.parent),
    ]);
  } catch {
    fail("output directory parent changed");
  }
  if (!status.isDirectory()
      || status.isSymbolicLink()
      || !sameObjectIdentity(output.parentStatus, status)
      || !samePath(output.parent, canonical)) {
    fail("output directory parent changed");
  }
}

async function requireAbsentOutput(outputPath) {
  try {
    await lstat(outputPath);
  } catch (error) {
    if (error?.code === "ENOENT") return;
    fail("output directory state is unavailable");
  }
  fail("output directory already exists");
}

export async function admitSigningKeyReference({ signingKeyPath, excludedRoots }) {
  let keyPath;
  try {
    keyPath = normalizedAbsolutePath(signingKeyPath, "signing key reference");
    if (!Array.isArray(excludedRoots) || excludedRoots.length === 0) throw new Error();
    const key = await admitRegularFile(keyPath, "signing key reference", ARTIFACT_BYTES_MAX);
    const canonicalKey = await realpath(key.path);
    for (const root of excludedRoots) {
      const normalizedRoot = path.resolve(root);
      let canonicalRoot;
      try {
        canonicalRoot = await realpath(normalizedRoot);
      } catch {
        canonicalRoot = normalizedRoot;
      }
      if (atOrBeneath(key.path, normalizedRoot)
          || atOrBeneath(canonicalKey, canonicalRoot)) {
        throw new Error();
      }
    }
    return Object.freeze({ path: key.path, status: key.status });
  } catch {
    fail("signing key reference is invalid");
  }
}

export function buildOfflineEnvironment(source = process.env) {
  const allowed = new Set([
    "APPDATA",
    "ALL_PROXY",
    "COMSPEC",
    "HOME",
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "LANG",
    "LC_ALL",
    "LOCALAPPDATA",
    "NO_PROXY",
    "PATH",
    "PATHEXT",
    "SYSTEMROOT",
    "TEMP",
    "TMP",
    "TMPDIR",
    "USERPROFILE",
    "WINDIR",
    "XDG_CACHE_HOME",
    "XDG_CONFIG_HOME",
  ]);
  const result = {};
  for (const [key, value] of Object.entries(source ?? {})) {
    const canonical = key.toUpperCase();
    if (allowed.has(canonical) && typeof value === "string") result[key] = value;
    if (canonical === "COSIGN_PASSWORD" && typeof value === "string") {
      result.COSIGN_PASSWORD = value;
    }
  }
  return result;
}

export function executeLocalCosignSigning(
  cosignPath,
  { artifactPath, statementPath, signingKeyPath, bundlePath, env },
  cosignRun = runCosign,
) {
  return cosignRun({
    cosignPath,
    args: [
      "attest-blob",
      "--statement", statementPath,
      "--key", signingKeyPath,
      "--bundle", bundlePath,
      "--tlog-upload=false",
      "--use-signing-config=false",
      "--yes",
      artifactPath,
    ],
    env: buildOfflineEnvironment(env),
    timeoutMs: COSIGN_TIMEOUT_MS,
    maxOutputBytes: COSIGN_OUTPUT_BYTES_MAX,
  });
}

export function parseArguments(argv) {
  if (!Array.isArray(argv)) fail("arguments are invalid");
  const values = new Map();
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!FLAGS.has(flag)
        || typeof value !== "string"
        || value.length === 0
        || FLAGS.has(value)
        || values.has(flag)) {
      fail("arguments are invalid");
    }
    values.set(flag, value);
  }
  if (argv.length % 2 !== 0 || REQUIRED_FLAGS.some((flag) => !values.has(flag))) {
    fail("arguments are invalid");
  }
  return Object.freeze({
    artifact: values.get("--artifact"),
    evidence: values.get("--evidence"),
    trustPolicy: values.get("--trust-policy"),
    producerId: values.get("--producer-id"),
    signingKey: values.get("--signing-key"),
    outputDirectory: values.get("--output-directory"),
    cosign: values.get("--cosign"),
  });
}

function buildVerificationRequest({
  artifact,
  evidence,
  evidenceBytes,
  evidenceName,
  evidenceSha256,
  bundle,
  bundleBytes,
  bundleName,
  bundleSha256,
  producer,
  policyPath,
  cosignPath,
  env,
}) {
  const row = {
    component_id: evidence.component_id,
    component_version: evidence.component_version,
    target_triple: evidence.target_triple,
    ownership: evidence.ownership,
    source_repository: evidence.source.repository,
    source_commit: evidence.source.commit,
    artifact: structuredClone(evidence.artifact),
    component_manifest_sha256: evidence.component_manifest_sha256,
    dependencies: [...evidence.dependencies],
    evidence: {
      format: "component_evidence_v1",
      name: evidenceName,
      sha256: evidenceSha256,
    },
    provenance: {
      producer_id: producer.producer_id,
      bundle_name: bundleName,
      bundle_sha256: bundleSha256,
    },
  };
  const component = {
    component_id: evidence.component_id,
    version: evidence.component_version,
    repository: evidence.source.repository,
    requires: [...evidence.dependencies],
  };
  return {
    assets: {
      artifact: {
        path: artifact.path,
        sizeBytes: Number(artifact.status.size),
        sha256: evidence.artifact.sha256,
      },
      evidence: {
        path: evidenceBytes.path,
        bytes: evidenceBytes.bytes,
        json: evidence,
        sizeBytes: evidenceBytes.bytes.byteLength,
        sha256: evidenceSha256,
      },
      bundle: {
        path: bundle.path,
        bytes: bundleBytes,
        sizeBytes: bundleBytes.byteLength,
        sha256: bundleSha256,
      },
    },
    row,
    component,
    producer,
    policyPath,
    cosignPath,
    env,
  };
}

function verifyStagedCandidate(request) {
  const verified = verifyLocalKeyedCandidate({
    assets: request.assets,
    row: request.row,
    component: request.component,
    producer: request.producer,
    trustPolicyPath: request.policyPath,
    cosignPath: request.cosignPath,
    validationMode: "official",
    env: request.env,
  });
  return compareVerifiedProvenance({
    verified,
    row: request.row,
    component: request.component,
    producer: request.producer,
  });
}

async function removeCreatedStaging(staging, output) {
  if (typeof staging !== "string"
      || path.dirname(staging) !== output.parent
      || !path.basename(staging).startsWith(`.${output.leaf}.staging-`)) {
    fail("staging cleanup identity is invalid");
  }
  try {
    await rm(staging, { recursive: true, force: true });
  } catch {
    fail("staging cleanup failed");
  }
}

export async function signLocalComponentProvenance(options, {
  cosignRun = runCosign,
  verifyCandidate = verifyStagedCandidate,
} = {}) {
  const output = await admitOutputDirectory(options?.outputDirectory);
  const artifact = await admitRegularFile(
    options?.artifact, "artifact", ARTIFACT_BYTES_MAX,
  );
  const evidenceFile = await admitRegularFile(
    options?.evidence, "evidence", BigInt(JSON_BYTES_MAX),
  );
  const policyFile = await admitRegularFile(
    options?.trustPolicy, "trust policy", BigInt(JSON_BYTES_MAX),
  );
  const evidenceBytes = await readAdmittedFile(evidenceFile, "evidence");
  const evidenceCandidate = parseUniqueJson(evidenceBytes, "evidence");
  const evidence = validateComponentReleaseEvidence(evidenceCandidate);
  if (!isDeepStrictEqual(evidence, evidenceCandidate)) {
    fail("evidence must already use canonical claim values");
  }
  const policyBytes = await readAdmittedFile(policyFile, "trust policy");
  const policy = validateProvenanceTrustPolicy(parseUniqueJson(policyBytes, "trust policy"));
  const producer = producerById(policy, options?.producerId);
  if (producer.kind !== "sigstore_keyed_local"
      || producer.evidence_format !== "component_evidence_v1"
      || producer.transparency_log !== "none") {
    fail("selected producer is not a local keyed producer");
  }
  requireProducerScope(producer, {
    componentId: evidence.component_id,
    sourceRepository: evidence.source.repository,
    ownership: evidence.ownership,
  });
  admitTrustedPublicKey({ trustPolicyPath: policyFile.path, producer });

  const artifactDigest = await digestArtifact(artifact);
  if (path.basename(artifact.path) !== evidence.artifact.name
      || artifactDigest.size !== BigInt(evidence.artifact.size_bytes)
      || artifactDigest.sha256 !== evidence.artifact.sha256) {
    fail("artifact differs from component evidence");
  }
  const evidenceName = safeLeaf(path.basename(evidenceFile.path), "evidence name");
  const evidenceSha256 = createHash("sha256").update(evidenceBytes).digest("hex");
  const bundleName = safeLeaf(
    `${evidence.artifact.name}.sigstore.json`, "bundle name",
  );
  if (evidenceName === bundleName) fail("output asset names must be distinct");

  const canonicalRepositoryRoot = await realpath(repositoryRoot);
  const signingKey = await admitSigningKeyReference({
    signingKeyPath: options?.signingKey,
    excludedRoots: [
      canonicalRepositoryRoot,
      path.dirname(artifact.path),
      path.dirname(evidenceFile.path),
      path.dirname(policyFile.path),
      output.parent,
      output.path,
    ],
  });
  const cosignPath = resolveCosignExecutable({ override: options?.cosign });
  const env = buildOfflineEnvironment(options?.env ?? process.env);

  let staging;
  try {
    await requireStableOutputParent(output);
    staging = await mkdtemp(path.join(output.parent, `.${output.leaf}.staging-`));
    await chmod(staging, 0o700);
    const stagedEvidencePath = path.join(staging, evidenceName);
    const statementPath = path.join(
      staging, `.statement-${randomBytes(16).toString("hex")}.json`,
    );
    const bundlePath = path.join(staging, bundleName);
    await writeFile(stagedEvidencePath, evidenceBytes, { flag: "wx", mode: 0o600 });
    const statement = buildComponentProvenanceStatement(evidenceCandidate);
    if (!isDeepStrictEqual(statement.predicate, evidenceCandidate)) {
      fail("statement predicate differs from component evidence");
    }
    await writeFile(statementPath, JSON.stringify(statement), { flag: "wx", mode: 0o600 });
    await requireStableAdmittedFile(artifact, "artifact");
    await requireStableAdmittedFile(signingKey, "signing key reference");
    executeLocalCosignSigning(cosignPath, {
      artifactPath: artifact.path,
      statementPath,
      signingKeyPath: signingKey.path,
      bundlePath,
      env,
    }, cosignRun);
    await requireStableAdmittedFile(signingKey, "signing key reference");
    try {
      await unlink(statementPath);
    } catch {
      fail("temporary statement cleanup failed");
    }

    const entries = await readdir(staging, { withFileTypes: true });
    const expectedNames = new Set([evidenceName, bundleName]);
    if (entries.length !== 2
        || entries.some((entry) =>
          !expectedNames.has(entry.name) || !entry.isFile() || entry.isSymbolicLink())) {
      fail("staging output inventory is invalid");
    }
    const stagedEvidence = await admitRegularFile(
      stagedEvidencePath, "staged evidence", BigInt(JSON_BYTES_MAX),
    );
    const stagedEvidenceBytes = await readAdmittedFile(stagedEvidence, "staged evidence");
    if (!stagedEvidenceBytes.equals(evidenceBytes)) fail("staged evidence differs from input");
    const bundle = await admitRegularFile(bundlePath, "bundle", BigInt(JSON_BYTES_MAX));
    const bundleBytes = await readAdmittedFile(bundle, "bundle");
    const bundleSha256 = createHash("sha256").update(bundleBytes).digest("hex");
    const verification = buildVerificationRequest({
      artifact,
      evidence,
      evidenceBytes: { path: stagedEvidence.path, bytes: stagedEvidenceBytes },
      evidenceName,
      evidenceSha256,
      bundle,
      bundleBytes,
      bundleName,
      bundleSha256,
      producer,
      policyPath: policyFile.path,
      cosignPath,
      env,
    });
    const verified = await verifyCandidate(verification);
    if (!verified || typeof verified !== "object" || Array.isArray(verified)
        || verified.component_id !== evidence.component_id
        || verified.component_version !== evidence.component_version
        || verified.target_triple !== evidence.target_triple
        || verified.artifact.name !== evidence.artifact.name
        || verified.artifact.sha256 !== evidence.artifact.sha256
        || verified.evidence.name !== evidenceName
        || verified.evidence.sha256 !== evidenceSha256
        || verified.bundle.name !== bundleName
        || verified.bundle.sha256 !== bundleSha256
        || verified.producer.producer_id !== producer.producer_id
        || verified.producer.key_epoch !== producer.key_epoch
        || verified.producer.audit_level !== "self_managed_key") {
      fail("immediate verification result differs from signed identity");
    }
    await requireStableAdmittedFile(artifact, "artifact");
    await requireAbsentOutput(output.path);
    await requireStableOutputParent(output);
    try {
      await rename(staging, output.path);
    } catch {
      fail("output directory activation failed");
    }
    staging = undefined;
    return Object.freeze({
      schema: "arena_local_provenance_sign_result_v1",
      state: "verified",
      component_id: evidence.component_id,
      component_version: evidence.component_version,
      target_triple: evidence.target_triple,
      artifact_name: evidence.artifact.name,
      artifact_sha256: evidence.artifact.sha256,
      evidence_name: evidenceName,
      evidence_sha256: evidenceSha256,
      bundle_name: bundleName,
      bundle_sha256: bundleSha256,
      producer_id: producer.producer_id,
      key_epoch: producer.key_epoch,
      audit_level: "self_managed_key",
      output_directory: output.path,
    });
  } catch (error) {
    if (staging !== undefined) await removeCreatedStaging(staging, output);
    throw error;
  }
}

export async function main(argv = process.argv.slice(2)) {
  const parsed = parseArguments(argv);
  const result = await signLocalComponentProvenance(parsed);
  process.stdout.write(`${JSON.stringify(result)}\n`);
  return result;
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    await main();
  } catch (error) {
    const message = String(error?.message ?? "local provenance signing failed").slice(0, 2048);
    process.stderr.write(`${message}\n`);
    process.exitCode = 1;
  }
}
