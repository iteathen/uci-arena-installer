import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";

import {
  assembleWindowsInstallerEnvelope,
  inspectWindowsInstallerEnvelope,
} from "./assemble-windows-installer-launcher.mjs";

function pe(label) {
  return Buffer.concat([Buffer.from("MZ", "ascii"), Buffer.from(label, "utf8")]);
}

test("Windows installer launcher envelope is deterministic and digest-bound", () => {
  const input = {
    stub: pe("stub"),
    personal: pe("personal"),
    allUsers: pe("all-users"),
  };
  const first = assembleWindowsInstallerEnvelope(input);
  const second = assembleWindowsInstallerEnvelope(input);
  assert.deepEqual(first, second);
  const inspected = inspectWindowsInstallerEnvelope(first);
  assert.equal(inspected.schema, "arena_windows_installer_envelope_v1");
  assert.equal(inspected.stub_size, input.stub.length);
  assert.equal(inspected.personal.offset, input.stub.length);
  assert.equal(inspected.personal.size, input.personal.length);
  assert.equal(
    inspected.personal.sha256,
    createHash("sha256").update(input.personal).digest("hex"),
  );
  assert.equal(
    inspected.all_users.offset,
    input.stub.length + input.personal.length,
  );
});

test("Windows installer launcher envelope rejects child corruption", () => {
  const envelope = assembleWindowsInstallerEnvelope({
    stub: pe("stub"),
    personal: pe("personal"),
    allUsers: pe("all-users"),
  });
  envelope[envelope.indexOf(Buffer.from("personal"))] ^= 0xff;
  assert.throws(
    () => inspectWindowsInstallerEnvelope(envelope),
    /digest mismatch/,
  );
});

test("Windows installer launcher envelope rejects a non-PE input", () => {
  assert.throws(
    () => assembleWindowsInstallerEnvelope({
      stub: Buffer.from("stub"),
      personal: pe("personal"),
      allUsers: pe("all-users"),
    }),
    /launcher stub is not a nonempty Windows PE image/,
  );
});
