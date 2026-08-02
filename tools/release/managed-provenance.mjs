#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  appendFileSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";

export const MANAGED_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/managed-component-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export const SIGSTORE_BUNDLE_MEDIA_TYPE =
  "application/vnd.dev.sigstore.bundle.v0.3+json";

const INSTALLER_REPOSITORY = "iteathen/uci-arena-installer";
const COMMIT_PATTERN = /^[0-9a-f]{40}$/u;

function canonicalJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function readJson(filename) {
  return JSON.parse(readFileSync(filename, "utf8"));
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function decodeCanonicalBase64(value) {
  if (typeof value !== "string"
      || value.length === 0
      || value.length % 4 !== 0
      || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  const decoded = Buffer.from(value, "base64");
  if (decoded.toString("base64") !== value) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  return decoded;
}

function readBundleStatement(bundlePath) {
  const bundle = readJson(bundlePath);
  if (bundle?.mediaType !== SIGSTORE_BUNDLE_MEDIA_TYPE) {
    throw new Error("unsupported Sigstore bundle media type");
  }
  if (typeof bundle?.verificationMaterial?.certificate?.rawBytes !== "string"
      || bundle.verificationMaterial.certificate.rawBytes.length === 0) {
    throw new Error("Sigstore bundle is missing its signing certificate");
  }
  const logEntries = bundle.verificationMaterial.tlogEntries;
  if (!Array.isArray(logEntries)
      || logEntries.length === 0
      || logEntries.some((entry) => !entry?.inclusionProof)) {
    throw new Error("Sigstore bundle is missing a transparency-log inclusion proof");
  }
  if (bundle?.dsseEnvelope?.payloadType !== "application/vnd.in-toto+json") {
    throw new Error("Sigstore bundle does not contain an in-toto DSSE payload");
  }
  if (!Array.isArray(bundle.dsseEnvelope.signatures)
      || bundle.dsseEnvelope.signatures.length !== 1) {
    throw new Error("Sigstore bundle must contain exactly one DSSE signature");
  }
  return JSON.parse(
    decodeCanonicalBase64(bundle.dsseEnvelope.payload).toString("utf8"),
  );
}

export function buildExpectedManagedStatement(managedResult) {
  return {
    _type: INTOTO_STATEMENT_TYPE,
    subject: [{
      name: managedResult.artifact_name,
      digest: { sha256: managedResult.artifact_sha256 },
    }],
    predicateType: MANAGED_PREDICATE_TYPE,
    predicate: managedResult,
  };
}

export function prepareManagedProvenance({
  artifactPath,
  managedResultPath,
  statementDirectory,
}) {
  const artifact = path.resolve(artifactPath);
  const resultPath = path.resolve(managedResultPath);
  const managedResult = readJson(resultPath);
  if (managedResult.schema !== "arena_managed_component_release_result_v1"
      || managedResult.release_authority !== "installer_managed_component_only"
      || managedResult.suite_installer_authority !== INSTALLER_REPOSITORY
      || managedResult.source_repository !== INSTALLER_REPOSITORY
      || !COMMIT_PATTERN.test(managedResult.source_commit ?? "")
      || path.basename(artifact) !== managedResult.artifact_name
      || sha256File(artifact) !== managedResult.artifact_sha256) {
    throw new Error("managed result does not bind the exact Installer artifact");
  }
  const statement = buildExpectedManagedStatement(managedResult);
  const statementRoot = path.resolve(statementDirectory);
  mkdirSync(statementRoot, { recursive: true });
  const bundlePath = `${artifact}.sigstore.json`;
  const statementPath = path.join(
    statementRoot,
    `${managedResult.artifact_name}.intoto.json`,
  );
  writeFileSync(statementPath, canonicalJson(statement), { flag: "wx" });
  return {
    artifactPath: artifact,
    bundlePath,
    predicateType: MANAGED_PREDICATE_TYPE,
    statement,
    statementPath,
  };
}

export function verifyBundlePayload(bundlePath, statementPath) {
  const actual = readBundleStatement(bundlePath);
  const expected = readJson(statementPath);
  if (!isDeepStrictEqual(actual, expected)) {
    throw new Error("Sigstore bundle payload does not match the expected statement");
  }
  return actual;
}

function parseArguments(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    values[flag.slice(2)] = value;
  }
  return values;
}

function writeGitHubOutputs(filename, values) {
  for (const [name, value] of Object.entries(values)) {
    const output = String(value);
    if (/[\u0000-\u001f\u007f]/u.test(output)) {
      throw new Error(`unsafe GitHub output value for ${name}`);
    }
    appendFileSync(filename, `${name}=${output}\n`, "utf8");
  }
}

function main(argv) {
  const [command, ...rest] = argv;
  const args = parseArguments(rest);
  if (command === "prepare") {
    const prepared = prepareManagedProvenance({
      artifactPath: args.artifact,
      managedResultPath: args.result,
      statementDirectory: args["statement-directory"],
    });
    if (args["github-output"]) {
      writeGitHubOutputs(args["github-output"], {
        artifact_path: prepared.artifactPath,
        bundle_path: prepared.bundlePath,
        predicate_type: prepared.predicateType,
        statement_path: prepared.statementPath,
      });
    } else {
      process.stdout.write(canonicalJson(prepared));
    }
    return;
  }
  if (command === "verify-bundle") {
    verifyBundlePayload(args.bundle, args.statement);
    process.stdout.write("managed-provenance: bundle payload verified\n");
    return;
  }
  throw new Error("usage: managed-provenance.mjs <prepare|verify-bundle> [options]");
}

if (process.argv[1]
    && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`managed-provenance: ${error.message}\n`);
    process.exitCode = 1;
  }
}
