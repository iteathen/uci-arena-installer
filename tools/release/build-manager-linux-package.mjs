#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";

import { enumerateComponentFiles } from "./component-artifact.mjs";
import { deterministicTarGz } from "./deterministic-tar.mjs";
import { stageManagerLinuxPackage } from "./manager-linux-payload.mjs";

const FORMATS = new Set(["deb", "rpm", "tar"]);

function parse(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    const key = flag.slice(2);
    if (Object.hasOwn(result, key)) throw new Error(`duplicate --${key}`);
    result[key] = value;
  }
  return result;
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

export function renderDebianControl(plan) {
  const size = [...plan.entries.values()].reduce(
    (sum, entry) => sum + statSync(entry.source).size, 0,
  );
  const packagePolicy = plan.policy.package;
  return [
    `Package: ${packagePolicy.name}`,
    `Version: ${plan.version}`,
    `Section: ${packagePolicy.section}`,
    `Priority: ${packagePolicy.priority}`,
    `Architecture: ${packagePolicy.architecture.deb}`,
    `Maintainer: ${packagePolicy.maintainer}`,
    `Depends: ${packagePolicy.deb_dependencies}`,
    `Installed-Size: ${Math.max(1, Math.ceil(size / 1024))}`,
    `Homepage: ${packagePolicy.homepage}`,
    `Description: ${packagePolicy.summary}`,
    ` ${packagePolicy.description}`,
    "",
  ].join("\n");
}

function rpmScript(filename) {
  return readFileSync(filename, "utf8").replace(/^#![^\r\n]*\r?\n/u, "").trimEnd();
}

export function renderRpmSpec(plan, stagedRoot) {
  const p = plan.policy.package;
  const fileRows = enumerateComponentFiles(stagedRoot).map((relative) => `/${relative}`);
  const shellRoot = `'${path.resolve(stagedRoot).replaceAll("'", "'\\''")}'`;
  return [
    `Name: ${p.name}`,
    `Version: ${plan.version}`,
    "Release: 1%{?dist}",
    `Summary: ${p.summary}`,
    `License: ${p.license}`,
    `URL: ${p.homepage}`,
    `Group: ${p.group}`,
    `Requires: ${p.rpm_dependencies}`,
    "BuildArch: x86_64",
    "%global _build_id_links none",
    "%description",
    p.description,
    "%prep",
    "%build",
    "%install",
    "rm -rf %{buildroot}",
    "mkdir -p %{buildroot}",
    `cp -a ${shellRoot}/. %{buildroot}/`,
    "%post",
    rpmScript(plan.maintainerScripts.rpm.post),
    "%preun",
    rpmScript(plan.maintainerScripts.rpm.preun),
    "%postun",
    rpmScript(plan.maintainerScripts.rpm.postun),
    "%files",
    ...fileRows,
    "",
  ].join("\n");
}

function runTool(executable, args, cwd) {
  const result = spawnSync(executable, args, {
    cwd,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    const detail = [result.error?.message, result.stderr, result.stdout]
      .filter((value) => typeof value === "string" && value.trim()).join("\n");
    throw new Error(`${path.basename(executable)} package adapter failed: ${detail}`);
  }
}

function copyDebianScripts(plan, root) {
  const control = path.join(root, "DEBIAN");
  mkdirSync(control, { recursive: true });
  writeFileSync(path.join(control, "control"), renderDebianControl(plan));
  for (const [name, source] of Object.entries(plan.maintainerScripts.deb)) {
    copyFileSync(source, path.join(control, name));
    chmodSync(path.join(control, name), 0o755);
  }
}

export function buildManagerLinuxPackage(options) {
  if (!FORMATS.has(options.format)) throw new Error("format must be deb, rpm, or tar");
  const epoch = Number(options.sourceDateEpoch);
  if (!Number.isSafeInteger(epoch) || epoch < 0) {
    throw new Error("source date epoch must be a nonnegative integer");
  }
  const output = path.resolve(options.output);
  const temporary = mkdtempSync(path.join(os.tmpdir(), "arena-manager-linux-"));
  const root = path.join(temporary, "root");
  const stagedOutput = `${output}.staging-${process.pid}`;
  try {
    const plan = stageManagerLinuxPackage({ ...options, outputRoot: root });
    mkdirSync(path.dirname(output), { recursive: true });
    rmSync(stagedOutput, { force: true });
    if (options.format === "tar") {
      writeFileSync(stagedOutput, deterministicTarGz(root, epoch));
    } else if (options.format === "deb") {
      copyDebianScripts(plan, root);
      runTool(options.dpkgDeb ?? "dpkg-deb", [
        "--build", "--root-owner-group", root, stagedOutput,
      ], temporary);
    } else {
      const top = path.join(temporary, "rpmbuild");
      const rpms = path.join(top, "RPMS");
      mkdirSync(path.join(top, "SPECS"), { recursive: true });
      mkdirSync(rpms, { recursive: true });
      const spec = path.join(top, "SPECS", "uci-arena-manager.spec");
      writeFileSync(spec, renderRpmSpec(plan, root));
      runTool(options.rpmbuild ?? "rpmbuild", [
        "-bb", "--define", `_topdir ${top}`, spec,
      ], temporary);
      const candidates = [];
      const collect = (directory) => {
        for (const entry of readdirSync(directory, { withFileTypes: true })) {
          const filename = path.join(directory, entry.name);
          if (entry.isDirectory()) collect(filename);
          else if (entry.isFile() && entry.name.endsWith(".rpm")) candidates.push(filename);
        }
      };
      collect(rpms);
      if (candidates.length !== 1) {
        throw new Error(`RPM adapter emitted ${candidates.length} package candidates`);
      }
      copyFileSync(candidates[0], stagedOutput);
    }
    if (!lstatSync(stagedOutput).isFile() || statSync(stagedOutput).size < 1) {
      throw new Error("Linux package adapter emitted no package bytes");
    }
    rmSync(output, { force: true });
    renameSync(stagedOutput, output);
    return {
      output,
      format: options.format,
      size_bytes: statSync(output).size,
      sha256: sha256File(output),
    };
  } finally {
    rmSync(stagedOutput, { force: true });
    rmSync(temporary, { recursive: true, force: true });
  }
}

function main() {
  const args = parse(process.argv.slice(2));
  for (const required of [
    "manager-root", "policy", "version", "format", "output", "source-date-epoch",
  ]) {
    if (!args[required]) throw new Error(`missing --${required}`);
  }
  process.stdout.write(`${JSON.stringify(buildManagerLinuxPackage({
    managerRoot: args["manager-root"],
    policyPath: args.policy,
    version: args.version,
    format: args.format,
    output: args.output,
    sourceDateEpoch: args["source-date-epoch"],
    dpkgDeb: args["dpkg-deb"],
    rpmbuild: args.rpmbuild,
  }))}\n`);
}

if (import.meta.url === `file://${process.argv[1]?.replaceAll("\\", "/")}`) {
  main();
}
