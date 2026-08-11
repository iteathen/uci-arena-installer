#!/usr/bin/env node

import {
  chmodSync,
  existsSync,
  lstatSync,
  mkdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

import { deterministicTarGz } from "./deterministic-tar.mjs";
import { enumerateComponentFiles } from "./component-artifact.mjs";

const MARKER = "__UCI_ARENA_PAYLOAD_BELOW__";

export function renderLinuxPortableHeader({ offline, development }) {
  const trust = development
    ? '  --allow-unsigned-development-catalog \\\n'
    : '  --metadata-dir "${temporary}/release/tuf" \\\n';
  const transport = offline ? "  --offline \\\n" : "";
  return [
    "#!/bin/sh",
    "set -eu",
    "",
    `marker="${MARKER}"`,
    'line="$(awk "/^${marker}\\$/ { print NR + 1; exit }" "$0")"',
    'if [ -z "${line}" ]; then',
    '  echo "UCI Arena portable payload marker is missing" >&2',
    "  exit 2",
    "fi",
    "",
    'temporary="$(mktemp -d "${TMPDIR:-/tmp}/uci-arena-setup.XXXXXX")"',
    "cleanup() {",
    '  rm -rf -- "${temporary}"',
    "}",
    "trap cleanup EXIT HUP INT TERM",
    "",
    'for argument in "$@"; do',
    '  case "${argument}" in',
    "    --catalog|--catalog=*|--metadata-dir|--metadata-dir=*|\\",
    "      --allow-unsigned-development-catalog|--offline)",
    '      echo "portable media owns catalog trust and transport options" >&2',
    "      exit 2",
    "      ;;",
    "  esac",
    "done",
    "",
    'tail -n +"${line}" "$0" | tar -xzf - -C "${temporary}"',
    'status=0',
    '"${temporary}/bin/arena-setup" \\',
    '  --catalog "${temporary}/release/catalog.json" \\',
    trust.trimEnd(),
    ...(transport ? [transport.trimEnd()] : []),
    '  "$@" || status=$?',
    'exit "${status}"',
    MARKER,
    "",
  ].join("\n");
}

function validateMedia(mediaRoot, development) {
  const media = path.resolve(mediaRoot);
  if (!lstatSync(media).isDirectory()
      || !lstatSync(path.join(media, "bin", "arena-setup")).isFile()
      || !lstatSync(path.join(media, "release", "catalog.json")).isFile()) {
    throw new Error("Linux portable media is incomplete");
  }
  const files = enumerateComponentFiles(media);
  if (files.some((relative) =>
    /(^|\/)secrets?(\/|$)|lichess[_-]?bot[_-]?token/iu.test(relative))) {
    throw new Error("Linux portable media must never contain credentials or secret paths");
  }
  const tufRoot = path.join(media, "release", "tuf", "root.json");
  if (!development && (!existsSync(tufRoot) || !lstatSync(tufRoot).isFile())) {
    throw new Error("official Linux portable media requires release/tuf/root.json");
  }
  return media;
}

export function buildLinuxPortableInstaller({
  mediaRoot,
  output,
  sourceDateEpoch,
  offline,
  development,
}) {
  const epoch = Number(sourceDateEpoch);
  if (!Number.isSafeInteger(epoch) || epoch < 0
      || typeof offline !== "boolean" || typeof development !== "boolean") {
    throw new Error("Linux portable installer requires explicit build inputs");
  }
  const media = validateMedia(mediaRoot, development);
  const destination = path.resolve(output);
  const staging = `${destination}.staging-${process.pid}`;
  const previous = `${destination}.previous-${process.pid}`;
  mkdirSync(path.dirname(destination), { recursive: true });
  const bytes = Buffer.concat([
    Buffer.from(renderLinuxPortableHeader({ offline, development }), "utf8"),
    deterministicTarGz(media, epoch),
  ]);
  try {
    rmSync(staging, { force: true });
    rmSync(previous, { force: true });
    writeFileSync(staging, bytes, { flag: "wx", mode: 0o755 });
    chmodSync(staging, 0o755);
    const hadPrevious = existsSync(destination);
    if (hadPrevious) renameSync(destination, previous);
    try {
      renameSync(staging, destination);
      if (hadPrevious) rmSync(previous, { force: true });
    } catch (error) {
      if (existsSync(destination)) rmSync(destination, { force: true });
      if (hadPrevious && existsSync(previous)) renameSync(previous, destination);
      throw error;
    }
    return { output: destination, size_bytes: statSync(destination).size };
  } finally {
    rmSync(staging, { force: true });
  }
}

function parse(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    if (!argv[index]?.startsWith("--") || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${argv[index] ?? "<end>"}`);
    }
    values[argv[index].slice(2)] = argv[index + 1];
  }
  for (const required of [
    "media-root", "output", "source-date-epoch", "offline", "development",
  ]) {
    if (values[required] === undefined) throw new Error(`missing --${required}`);
  }
  for (const flag of ["offline", "development"]) {
    if (!["true", "false"].includes(values[flag])) {
      throw new Error(`--${flag} must be true or false`);
    }
  }
  return values;
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    const args = parse(process.argv.slice(2));
    const result = buildLinuxPortableInstaller({
      mediaRoot: args["media-root"],
      output: args.output,
      sourceDateEpoch: args["source-date-epoch"],
      offline: args.offline === "true",
      development: args.development === "true",
    });
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } catch (error) {
    process.stderr.write(`build-linux-portable-installer: ${error.message}\n`);
    process.exitCode = 1;
  }
}
