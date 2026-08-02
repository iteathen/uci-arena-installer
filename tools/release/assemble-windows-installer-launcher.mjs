#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  lstatSync,
  mkdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

export const envelopeFooterSize = 128;
const envelopeMagic = Buffer.from("UCIARENAINSTV1!\0", "ascii");

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest();
}

function requirePe(name, bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length < 2 ||
      bytes[0] !== 0x4d || bytes[1] !== 0x5a) {
    throw new Error(`${name} is not a nonempty Windows PE image`);
  }
}

export function assembleWindowsInstallerEnvelope({
  stub,
  personal,
  allUsers,
}) {
  requirePe("launcher stub", stub);
  requirePe("Personal bundle", personal);
  requirePe("All users bundle", allUsers);
  const personalOffset = stub.length;
  const allUsersOffset = personalOffset + personal.length;
  for (const [name, value] of Object.entries({
    stubSize: stub.length,
    personalOffset,
    personalSize: personal.length,
    allUsersOffset,
    allUsersSize: allUsers.length,
  })) {
    if (!Number.isSafeInteger(value) || value < 1) {
      throw new Error(`${name} exceeds the version-1 envelope capacity`);
    }
  }
  const footer = Buffer.alloc(envelopeFooterSize);
  envelopeMagic.copy(footer, 0);
  footer.writeUInt32LE(1, 16);
  footer.writeBigUInt64LE(BigInt(stub.length), 24);
  footer.writeBigUInt64LE(BigInt(personalOffset), 32);
  footer.writeBigUInt64LE(BigInt(personal.length), 40);
  sha256(personal).copy(footer, 48);
  footer.writeBigUInt64LE(BigInt(allUsersOffset), 80);
  footer.writeBigUInt64LE(BigInt(allUsers.length), 88);
  sha256(allUsers).copy(footer, 96);
  return Buffer.concat([stub, personal, allUsers, footer]);
}

export function inspectWindowsInstallerEnvelope(bytes) {
  if (!Buffer.isBuffer(bytes) || bytes.length < envelopeFooterSize) {
    throw new Error("installer envelope is truncated");
  }
  const footerOffset = bytes.length - envelopeFooterSize;
  const footer = bytes.subarray(footerOffset);
  if (!footer.subarray(0, envelopeMagic.length).equals(envelopeMagic) ||
      footer.readUInt32LE(16) !== 1 || footer.readUInt32LE(20) !== 0) {
    throw new Error("installer envelope identity or version is invalid");
  }
  const asNumber = (offset) => {
    const value = footer.readBigUInt64LE(offset);
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new Error("installer envelope offset exceeds JavaScript capacity");
    }
    return Number(value);
  };
  const stubSize = asNumber(24);
  const personalOffset = asNumber(32);
  const personalSize = asNumber(40);
  const allUsersOffset = asNumber(80);
  const allUsersSize = asNumber(88);
  if (stubSize < 1 || personalSize < 1 || allUsersSize < 1 ||
      personalOffset !== stubSize ||
      allUsersOffset !== personalOffset + personalSize ||
      allUsersOffset + allUsersSize !== footerOffset) {
    throw new Error("installer envelope payload boundaries are invalid");
  }
  const personal = bytes.subarray(personalOffset, allUsersOffset);
  const allUsers = bytes.subarray(allUsersOffset, footerOffset);
  if (!sha256(personal).equals(footer.subarray(48, 80)) ||
      !sha256(allUsers).equals(footer.subarray(96, 128))) {
    throw new Error("installer envelope payload digest mismatch");
  }
  return {
    schema: "arena_windows_installer_envelope_v1",
    stub_size: stubSize,
    personal: {
      offset: personalOffset,
      size: personalSize,
      sha256: footer.subarray(48, 80).toString("hex"),
    },
    all_users: {
      offset: allUsersOffset,
      size: allUsersSize,
      sha256: footer.subarray(96, 128).toString("hex"),
    },
  };
}

function parse(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || !value) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    result[flag.slice(2)] = value;
  }
  for (const required of ["stub", "personal", "all-users", "output"]) {
    if (!result[required]) throw new Error(`--${required} is required`);
  }
  return result;
}

function exactInput(filename, label) {
  const resolved = path.resolve(filename);
  const status = lstatSync(resolved);
  if (!status.isFile() || status.isSymbolicLink() || status.size < 1) {
    throw new Error(`${label} must be a nonempty regular file`);
  }
  return resolved;
}

export function main(argv = process.argv.slice(2)) {
  const args = parse(argv);
  const stub = exactInput(args.stub, "launcher stub");
  const personal = exactInput(args.personal, "Personal bundle");
  const allUsers = exactInput(args["all-users"], "All users bundle");
  const output = path.resolve(args.output);
  if ([stub, personal, allUsers].includes(output)) {
    throw new Error("launcher output must not replace an input");
  }
  const envelope = assembleWindowsInstallerEnvelope({
    stub: readFileSync(stub),
    personal: readFileSync(personal),
    allUsers: readFileSync(allUsers),
  });
  inspectWindowsInstallerEnvelope(envelope);
  mkdirSync(path.dirname(output), { recursive: true });
  const staging = `${output}.staging-${process.pid}`;
  try {
    writeFileSync(staging, envelope, { flag: "wx" });
    rmSync(output, { force: true });
    renameSync(staging, output);
  } finally {
    rmSync(staging, { force: true });
  }
  return inspectWindowsInstallerEnvelope(envelope);
}

if (path.resolve(process.argv[1] ?? "") === fileURLToPath(import.meta.url)) {
  try {
    const result = main();
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } catch (error) {
    process.stderr.write(`assemble-windows-installer-launcher: ${error.message}\n`);
    process.exitCode = 1;
  }
}
