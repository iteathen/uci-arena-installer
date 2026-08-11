import assert from "node:assert/strict";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  buildLinuxPortableInstaller,
  renderLinuxPortableHeader,
} from "./build-linux-portable-installer.mjs";

function fixture() {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-linux-portable-"));
  const media = path.join(root, "media");
  mkdirSync(path.join(media, "bin"), { recursive: true });
  mkdirSync(path.join(media, "release"), { recursive: true });
  writeFileSync(path.join(media, "bin", "arena-setup"), "setup", { mode: 0o755 });
  writeFileSync(path.join(media, "release", "catalog.json"), "{}\n");
  return { root, media };
}

test("Linux development headers separate trust, transport, and cleanup", () => {
  const online = renderLinuxPortableHeader({ offline: false, development: true });
  const offline = renderLinuxPortableHeader({ offline: true, development: true });
  assert.match(online, /--allow-unsigned-development-catalog/u);
  assert.doesNotMatch(online, /^  --metadata-dir /mu);
  assert.doesNotMatch(online, /^  --offline \\/mu);
  assert.match(offline, /--allow-unsigned-development-catalog/u);
  assert.match(offline, /--offline/u);
  assert.match(offline, /status=0/u);
  assert.match(offline, /trap cleanup EXIT/u);
  assert.match(offline, /portable media owns catalog trust and transport options/u);
  assert.match(offline, /--metadata-dir=\*/u);
  assert.doesNotMatch(offline, /exec "\$\{temporary\}\/bin\/arena-setup"/u);
});

test("Linux portable media is deterministic and official mode requires TUF", (t) => {
  const f = fixture();
  t.after(() => rmSync(f.root, { recursive: true, force: true }));
  const first = path.join(f.root, "first.run");
  const second = path.join(f.root, "second.run");
  for (const output of [first, second]) {
    buildLinuxPortableInstaller({
      mediaRoot: f.media,
      output,
      sourceDateEpoch: 1_700_000_000,
      offline: false,
      development: true,
    });
  }
  assert.deepEqual(readFileSync(first), readFileSync(second));
  assert.throws(() => buildLinuxPortableInstaller({
    mediaRoot: f.media,
    output: path.join(f.root, "official.run"),
    sourceDateEpoch: 1_700_000_000,
    offline: false,
    development: false,
  }), /requires release\/tuf\/root\.json/u);
  assert.equal(existsSync(path.join(f.root, "official.run")), false);
});

test("Linux portable media rejects credential-shaped paths before publication", (t) => {
  const f = fixture();
  t.after(() => rmSync(f.root, { recursive: true, force: true }));
  mkdirSync(path.join(f.media, "secrets"));
  writeFileSync(path.join(f.media, "secrets", "operator-token"), "never package this");
  const output = path.join(f.root, "blocked.run");
  writeFileSync(output, "accepted previous media");
  assert.throws(() => buildLinuxPortableInstaller({
    mediaRoot: f.media,
    output,
    sourceDateEpoch: 1_700_000_000,
    offline: true,
    development: true,
  }), /must never contain credentials/u);
  assert.equal(readFileSync(output, "utf8"), "accepted previous media");
});
