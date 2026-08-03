import { readFileSync, statSync } from "node:fs";
import path from "node:path";
import { gzipSync } from "node:zlib";

import { enumerateComponentFiles } from "./component-artifact.mjs";

function writeOctal(buffer, offset, length, value) {
  const encoded = Math.trunc(value).toString(8).padStart(length - 1, "0");
  if (encoded.length >= length) throw new Error(`tar numeric field overflow: ${value}`);
  buffer.write(encoded, offset, length - 1, "ascii");
  buffer[offset + length - 1] = 0;
}

function splitTarPath(relative) {
  if (Buffer.byteLength(relative) <= 100) return { name: relative, prefix: "" };
  for (
    let index = relative.lastIndexOf("/");
    index > 0;
    index = relative.lastIndexOf("/", index - 1)
  ) {
    const prefix = relative.slice(0, index);
    const name = relative.slice(index + 1);
    if (Buffer.byteLength(prefix) <= 155 && Buffer.byteLength(name) <= 100) {
      return { name, prefix };
    }
  }
  throw new Error(`archive path exceeds deterministic ustar limits: ${relative}`);
}

function tarHeader(relative, size, mode, epoch) {
  const { name, prefix } = splitTarPath(relative);
  const header = Buffer.alloc(512);
  header.write(name, 0, 100, "utf8");
  writeOctal(header, 100, 8, mode);
  writeOctal(header, 108, 8, 0);
  writeOctal(header, 116, 8, 0);
  writeOctal(header, 124, 12, size);
  writeOctal(header, 136, 12, epoch);
  header.fill(0x20, 148, 156);
  header[156] = 0x30;
  header.write("ustar\0", 257, 6, "ascii");
  header.write("00", 263, 2, "ascii");
  header.write("root", 265, 32, "ascii");
  header.write("root", 297, 32, "ascii");
  header.write(prefix, 345, 155, "utf8");
  const checksum = header.reduce((sum, value) => sum + value, 0);
  header.write(checksum.toString(8).padStart(6, "0"), 148, 6, "ascii");
  header[154] = 0;
  header[155] = 0x20;
  return header;
}

export function deterministicTarGz(root, epoch) {
  const chunks = [];
  for (const relative of enumerateComponentFiles(root)) {
    const filename = path.join(root, ...relative.split("/"));
    const bytes = readFileSync(filename);
    const mode = (statSync(filename).mode & 0o111) !== 0 ? 0o755 : 0o644;
    chunks.push(tarHeader(relative, bytes.length, mode, epoch), bytes);
    const padding = (512 - (bytes.length % 512)) % 512;
    if (padding) chunks.push(Buffer.alloc(padding));
  }
  chunks.push(Buffer.alloc(1024));
  return gzipSync(Buffer.concat(chunks), { level: 9, mtime: 0 });
}
