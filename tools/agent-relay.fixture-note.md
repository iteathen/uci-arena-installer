Relay verification performed before publication:

- `node --check` passed for the published runner blob.
- `node tools/agent-relay.mjs --self-test` equivalent passed against the exact published content.
- An isolated local fixture used a temporary Git repository, bare remote, fake GitHub CLI, and fake Codex process. The fake Spark changed one allowed path but did not commit; the relay alone staged and committed the change, verified ancestry/scope/remote head, pushed the target branch, and emitted a result with `push_status: pushed_and_verified`.

This note records development evidence only. It is not Installer runtime/native acceptance evidence and must not be used to satisfy FI/ED/ACQ/ARC issue gates.
