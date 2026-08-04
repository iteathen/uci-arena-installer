# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot time: 2026-08-03 19:08 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the canonical continuation state. Fetch every current head before trusting a recorded SHA.

## Governing rules

- One authority per concern; project-owned neutral LEGO contracts at boundaries.
- Branch ancestry is product correctness.
- Exact bounded bytes, identities, schemas, paths, lifetimes, and cleanup.
- No peer-source imports, hidden discovery, or provider-private ownership crossing repositories.
- Jules output is hostile input until ancestry, every changed file, omission, race, test, cleanup path, and performance property are reviewed.
- Automatic CI remains disabled. Source review and isolated probes do not establish native/package/release readiness.

## Current conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness or release-readiness conclusion may be issued.

## Installer

Authoritative base: PR #2 `agent/release-assembly`.

Review stack:

- PR #3 bounded indexed assets → #2;
- PR #4 native process-tree stop → #2;
- PR #5 typed service actions → #4;
- PR #11 platform-neutral service results → #5, head `c40edfcbe12a5e5187a4b1543aee610f40d47f02`.

PR #11 is draft/mergeable and locally strict-compiled on Linux. Real systemd and Windows SCM builds/integration remain required. Issues #7–#10 still block package/lifecycle conclusions: exact stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

## Engine

Authoritative ancestry:

1. PR #39 `agent/system-sanity`
2. PR #44 `agent/model-adapter-contract`
3. PR #47 `agent/integrate-engine-audit-stacks`, SHA `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`

PR #52 remains a reviewable child. Inference #57/#45/#46, TT #43/#54, and performance #56/#58/#59 remain open.

### Active Jules session — issue #62 / PR #63

- control: `iteathen/Jules-control#1`
- session: `sessions/13034909149691235140`
- exact API starting branch: `agent/integrate-engine-audit-stacks`
- exact starting SHA: `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- output PR: Engine #63, draft, correct base
- branch: `jules-13034909149691235140-b7c89938`
- first head: `a5f773cb188dafd1e2fcbee0f638cad9a1aed8e7`
- second head: `7d3f8c1f0fec3b0628b0ef3bf07f6476ddb826be`
- current state: second correction in progress

First hostile review comment `5173513437` rejected weak file admission, optional identity, noncanonical time, lost malformed demand, untyped overflow, duplicate authority, abort/cleanup defects, weak tests, and missing truthful spec.

Second hostile review comment `5173734334` rejected the corrected head for remaining defects:

- link-following/no Windows reparse-safe object admission;
- replacement-unsafe deletion;
- unbounded `readdirSync()` and per-candidate allocation before request limits;
- non-exact UTC round trip;
- incomplete filename/packet/UUID/byte identity;
- malformed/oversized foreground demand still disappearing;
- fabricated overflow producer/nonce and wrong reason;
- acquisition/snapshot/demand ignoring overflow;
- holder and foreground sampled with different clocks;
- watcher/timer/abort cleanup and allocation evidence still incomplete;
- governing spec claiming protections not actually implemented.

The second correction message and `/jules continue` were accepted. Keep PR #63 draft. Review only after its head moves beyond `7d3f8c1f…`; if Jules stops unchanged, issue `/jules continue` again.

## Manager

Authoritative base: PR #54 `agent/release-assembly`, SHA `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

Reviewable stack:

### PR #78 — pure UCI identity/option contract

- branch `agent/uci-option-contract-v1`
- head `9e18b058b4870b02f65b08fadb8d6f14df85d97b`
- strict UTF-8 scalar identity/option grammar, type-specific fields, cumulative bounds, ASCII-fold uniqueness, canonical override map
- hostile review fixed cumulative metadata bypass for externally supplied typed option records
- strict compile/adversarial probes passed; representative 256-option parse measured 522 allocations

### PR #79 — process adoption

- branch `agent/integrate-uci-option-contract`
- head `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`
- exact parser adoption, canonical advertised option spelling, metadata bounds before accumulation, process regressions
- raw framing and launch identity deliberately left open

### PR #80 — bounded UCI wire framing

- branch `agent/bound-uci-probe-wire-framing`
- head `683d7ae20a161d0b40d176d82173a87a961c77ea`
- draft, mergeable, zero behind PR #79, twelve ownership files
- `NativeProcessIoLimits` owns input/output/queue bounds and exact droppable prefix
- Windows/Linux frame bounded raw bytes; only exact `info ` may drop
- control overflow/queue pressure is fatal; control lines are never silently evicted
- complete partial writes; NUL/CR/LF input rejection; fatal reader state is observable
- UCI line policy: 4096-byte lines, 2048 queued lines, 512 KiB, exact UTF-8 scalar/control admission, exact `uciok`/`readyok`
- strict Linux compile, pure wire probe, and real-child framing probe passed
- full native Windows/Linux suites remain required

Issue #70 still has one final boundary: exact executable/working-directory/hash-to-launched-object identity.

Prepared branch `agent/bind-verified-uci-launch-identity` starts exactly at PR #80 head. Task files:

- `docs/audits/jules-workbenches/manager-70-verified-launch-task.md`
- `docs/audits/jules-workbenches/manager-70-verified-launch.yaml`

Required design includes Linux no-follow descriptor hashing plus `fexecve`/`execveat` and enforceable mutation prevention, and Windows reparse-safe deny-write/delete handles retained through `CreateProcessW` plus process-image identity verification. It is queued, not launched.

Manager PR #67 storage remains blocked. LocalAuth #69 remains open.

## Evidence Service

Authoritative stack ends at PR #22.

PR #23 is only a projector slice, not issue #20 completion. `Catalog.status()` must own the projection with one captured clock; wrappers must be removed and a real temporary-catalog test plus Node 24.15 checks added.

An exact branch-pinned Jules task is prepared but not launched:

- source branch `agent/project-daemon-heartbeat-staleness`
- expected SHA `3991983c397a222871cba70c6232da1a8be5b265`
- existing PR #23
- task/manifest under `docs/audits/jules-workbenches/evidence-20-*`

Issue #21 remains blocked on accepted Engine #62 and requires one absolute 2000 ms deadline from earliest foreground visibility through verified process-tree termination and immediate lease release.

## Book Forge

PRs #8/#11/#10 remain blocked. Issues #12–#14 remain open.

Issue #12 branch `agent/exact-acquisition-credential-use` starts exactly at PR #11 head `f698d49b6f9b7bdf8105dde3c598b053502e08f0`.

Prepared task requires callback-scoped exact credential bytes, POSIX no-follow descriptor identity, and a Windows handle-holding private IPC protocol that keeps the verified credential object authoritative through HTTP callback use. Task/manifest are under `docs/audits/jules-workbenches/book-forge-12-*`. It is queued, not launched.

Issues #13 and #14 still require recoverable snapshot publication and exact catalog/64-shard SQLite authority.

## Lichess Bot

PR #3 remains blocked. Continue in ownership order:

1. #4 exact file/publication/launch identity;
2. #5 raw-byte UCI and immutable qualification identity;
3. #6 durable activation and off-loop latest-wins status;
4. #7 exact SQLite foundation.

## Jules control and queue

Preferred transport: official Jules REST API through private `iteathen/Jules-control`.

Owner commands:

- `/jules status`
- `/jules activities`
- `/jules approve`
- `/jules continue`
- `/jules send <message>`

`/jules continue` uses dedicated `scripts/jules_continue.py` and is proven to recover false completion/feedback stops while preserving branch, scope, plan, and corrective instructions.

Queue in `docs/audits/jules-workbenches/registry.yaml`:

1. active Engine #62 correction;
2. Evidence #20 Catalog integration;
3. Manager #70 verified launch identity;
4. Book Forge #12 exact credential use.

Do not launch a queued broad task while Engine #62 remains active.

## Immediate restart order

1. Poll Engine PR #63. If its head changes beyond `7d3f8c1f…`, review the correction delta then the full result. If the session stops unchanged, `/jules continue`.
2. Update the Jules registry with every new head/review disposition.
3. Native-validate Installer PR #11 and Manager PRs #78–#80.
4. After Engine #62 is accepted or terminally rejected, launch Evidence #20 through the API queue.
5. Then run Manager verified-launch and Book Forge credential tasks in queue order.
6. Continue Evidence #21, Manager #69/storage, Lichess #4–#7, Book Forge #13/#14, and Installer #7–#10.
7. Update both handoffs, registry, and Installer PR #2 discoverability comment before stopping.

## Final gates

No soundness decision until all specs/preflight/boundary/substitution checks, native Windows/Linux builds, Node 24.15 provider suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifact/provenance closure, and full install-through-purge repeated lifecycle tests pass.
