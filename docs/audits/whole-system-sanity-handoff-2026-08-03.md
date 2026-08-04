# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot time: 2026-08-03 18:18 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: Engine, Manager, Lichess Bot, Book Forge, Evidence Service, and Installer

This file and its YAML companion are the canonical continuation state. Fetch every current head before trusting a recorded SHA.

## 1. Governing method

Read in order:

1. `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`
2. `docs/audits/whole-system-sanity-2026-08.md`
3. performance and branch-topology addenda
4. this handoff and its YAML companion
5. `docs/audits/jules-workbenches/registry.yaml`
6. each touched repository's `AGENTS.md`, read-first spec, registry, issue, and PR review

Non-negotiable rules:

- one authority per concern and project-owned neutral LEGO contracts;
- authoritative branch ancestry is product correctness;
- no peer-source imports or hidden discovery across repositories;
- exact bounded types, bytes, identities, schemas, paths, and lifecycle authority;
- no avoidable allocation, copying, filesystem, JSON, logging, or blocking in hot paths;
- Jules output is hostile input until ancestry, every changed file, call site, omission, test, race, cleanup path, and performance property are reviewed;
- repair by coherent ownership boundary, not arbitrary file count;
- automatic CI remains disabled; source review cannot substitute for native, packaged, recovery, and performance evidence.

## 2. Environment limits

Available here:

- connected private GitHub read/write, ancestry comparison, issues, PRs, and comments;
- Linux C++20 compiler for isolated strict-warning probes;
- Node 22.16 for isolated pure JavaScript checks;
- official Jules REST API through the private `iteathen/Jules-control` bridge.

Unavailable here:

- complete private checkouts and GitHub CLI authentication;
- required Node 24.15 full provider environments;
- Windows SDK/SCM and booted Linux systemd hosts;
- complete ONNX-linked Engine and installed-suite environments;
- full Installer lifecycle environment.

No final evidence exists for full Windows/Linux builds, real ONNX parity/cancellation/reload, representative search benchmarks, storage recovery matrices, or clean install through purge cycles.

## 3. Current conclusion

The audit is not complete. `PKG-E2E-011` remains unknown. No soundness or release-readiness statement may be issued.

## 4. Repository state

### 4.1 Installer

Authoritative base: PR #2 `agent/release-assembly`.

Review stack:

- PR #3 bounded indexed-asset results → #2;
- PR #4 native process-tree stop → #2;
- PR #5 typed service-control actions → #4;
- PR #11 platform-neutral service-control results → #5.

PR #11:

- branch `agent/platform-neutral-service-control-result`;
- head `c40edfcbe12a5e5187a4b1543aee610f40d47f02`;
- draft, mergeable, zero behind PR #5, six ownership files;
- neutral states include `unknown`, `unavailable`, `inactive`, `activating`, `active`, `deactivating`, `failed`;
- bounded printable service identity and diagnostics;
- shared pure outcome authority used by Linux and Windows adapters;
- Linux process failures are typed, systemd `LoadState` and `ActiveState` are distinct, interactive authorization is disabled;
- Windows handles are RAII-owned and missing/RPC/service-specific errors map to neutral results.

Validation: strict Linux compile and pure action/state matrix passed. Real systemd and Windows SCM builds/integration remain required.

Critical package/lifecycle blockers remain issues #7–#10: exact stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

### 4.2 Engine

Authoritative stack:

1. PR #39 `agent/system-sanity`
2. PR #44 `agent/model-adapter-contract`
3. PR #47 `agent/integrate-engine-audit-stacks`, head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`

Reviewable child: PR #52 canonical FEN admission → #47.

Inference remains open: issues #57, #45, #46. Jules PRs #60/#61 were rejected and closed. TT issues #43/#54 and performance issues #56/#58/#59 remain open.

#### Active Jules API session for issue #62

- source issue: Engine #62, timestamped typed foreground revocation;
- control repo/issue: `iteathen/Jules-control#1`;
- session: `sessions/13034909149691235140`;
- source repository: `iteathen/UCI_Arena`;
- exact API starting branch: `agent/integrate-engine-audit-stacks`;
- exact starting SHA: `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`;
- automation: `AUTO_CREATE_PR`;
- plan approval required;
- current state at this snapshot: `IN_PROGRESS`;
- prepared destination branch `agent/timestamp-resource-lease-revocation` remains byte-identical to PR #47; no Jules commit or PR is visible yet.

The first detailed Jules plan was rejected before implementation because it proposed:

- `statSync` followed by a path read, creating a packet TOCTOU race;
- clamping oversized producer identity, silently changing authority;
- scanning only the first 100 request files, which could miss the deterministic earliest request;
- two public wait names without demonstrated compatibility need.

Corrective instructions require one exact descriptor/handle read with byte bound before JSON decode, rejection rather than clamping, conservative typed overflow or a provably complete bounded earliest-selection algorithm, one canonical wait API, exact original `requested_at`, and conservative malformed/future timestamp handling.

Jules briefly reported `COMPLETED`, then `AWAITING_USER_FEEDBACK`, without a PR. The recovery command successfully returned it to `IN_PROGRESS`.

### 4.3 Manager

Authoritative base: PR #54 `agent/release-assembly`, SHA `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

Reviewable:

- PR #73 bounded match-plan admission → #54;
- PR #78 pure UCI identity/option contract → #54;
- PR #79 process integration → #78.

PR #78:

- branch `agent/uci-option-contract-v1`;
- head `9e18b058b4870b02f65b08fadb8d6f14df85d97b`;
- draft, mergeable, six files;
- strict UTF-8 scalar admission, exact identity cardinality, exact type-specific UCI grammar, cumulative bounds, ASCII-fold uniqueness, typed override validation, deterministic canonical spelling;
- hostile review fixed a cumulative-metadata bypass for externally supplied typed option records;
- strict compile and adversarial probes passed; representative 256-button parse measured 522 allocations.

PR #79:

- branch `agent/integrate-uci-option-contract`;
- head `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`;
- draft, mergeable, zero behind PR #78, four changed files;
- removes permissive identity overwrite and `istringstream` option parsing from `UciProcess`;
- parses complete metadata once through PR #78's production contract;
- applies 4096-byte line and 1024-line bounds before accumulating metadata;
- sends overrides using exact advertised spelling;
- focused process tests cover duplicate identity, repeated fields, acquisition amplification, and case-insensitive canonical application.

Issue #70 remains open for raw-byte framing and exact executable/path/hash launch identity. PR #67 storage remains blocked. LocalAuth issue #69 remains open.

### 4.4 Evidence Service

Authoritative stack through PR #22.

PR #23 is a partial projector slice, not an issue #20 closure:

- branch `agent/project-daemon-heartbeat-staleness`;
- head `3991983c397a222871cba70c6232da1a8be5b265`;
- draft, mergeable;
- pure timestamp/age projector and CLI/Manager wrappers;
- isolated Node 22 tests: 5 passed.

Remaining: `Catalog.status()` itself must own the projection with one captured clock, redundant wrappers must be removed, and a direct temporary-catalog test plus Node 24.15 full checks are required. The connector cannot safely narrow-patch the roughly 750-line catalog file.

Issue #21 remains blocked on accepted Engine #62 code and requires one absolute 2000 ms deadline from earliest visibility through verified process-tree termination and immediate lease release.

### 4.5 Book Forge

PRs #8/#11/#10 remain blocked. Issues:

- #12 exact credential bytes through one use-time descriptor/handle, no trim/cache, physical ownership reproof;
- #13 one recoverable snapshot build/publication operation and receipt;
- #14 exact catalog and 64-shard SQLite/WAL/SHM/schema/migration/recovery authority.

Reopened ledger rows: `BOOK-TRUST-017`, `BOOK-AUTH-018`, `BOOK-SCALE-020`.

### 4.6 Lichess Bot

PR #3 remains blocked. Ownership order:

1. issue #4 exact file/publication/launch identity;
2. issue #5 raw-byte UCI and immutable qualification identity;
3. issue #6 durable activation and off-loop latest-wins status;
4. issue #7 exact SQLite foundation.

No candidate promotion until process, API, storage, startup, latency, package, race, and recovery evidence pass.

## 5. Jules API control protocol

Preferred transport is the official Jules REST API through private repo `iteathen/Jules-control`.

Security:

- API key exists only as environment secret `JULES_API_KEY` in environment `jules`;
- never place it in chat, issues, logs, files, argv, or prompts;
- the bridge resolves exactly one Jules source and validates the requested `startingBranch` before session creation;
- only repository owner commands are accepted.

Control commands:

- `/jules status`
- `/jules activities`
- `/jules approve`
- `/jules continue`
- `/jules send <message>`

`/jules continue` is implemented by dedicated `scripts/jules_continue.py`. Use it when Jules reports a recoverable/non-substantive failure, `AWAITING_USER_FEEDBACK`, or apparent completion without a valid output. It instructs Jules to retry while preserving exact branch, scope, approved plan, and later corrections. A concrete unrecoverable blocker still requires exact evidence.

Temporary private workbenches are fallback only when the API cannot expose or accept the authoritative branch. The planned Engine #62 workbench was superseded before creation; there is no repository to delete.

## 6. Immediate restart order

1. Monitor Jules control issue #1. If it stops without a valid PR, use `/jules continue`.
2. When an Engine PR appears, freeze base/head, verify merge-base equals PR #47 SHA, inspect every patch, and hostile-review the corrected bounds/timestamp/wait semantics.
3. Update the Jules registry with output PR/head and review disposition; close control issue only after terminal state is recorded.
4. Run full native suites for Installer PR #11 and Manager PRs #78/#79.
5. Complete Evidence `Catalog.status()` ownership using a real checkout.
6. After accepted Engine #62 code is available, implement Evidence #21.
7. Continue Manager #69 and remaining #70 work, then reconstruct Manager storage PR #67.
8. Continue Lichess #4–#7 and Book Forge #12–#14 by ownership order.
9. Resolve Installer #7–#10 before any package/lifecycle conclusion.
10. Update this handoff, YAML, registry, exact validation, and Installer PR #2 discoverability comment before stopping.

## 7. Final decision gates

Before any soundness decision:

- all specs, preflight, boundary checks, and substitution probes pass;
- one authoritative ancestry per repository and no orphan fixes;
- complete Windows/Linux Engine, Manager, and Installer builds/CTest;
- required Node 24.15 provider suites and installed-package behavior;
- real ONNX, TT, storage, process, race, recovery, and performance evidence;
- immutable artifacts, manifests, receipts, digests, provenance, SBOMs, and dependency closure;
- clean install, use, repair, upgrade, reinstall, crash recovery, uninstall, purge, and repeated-cycle debris checks.

No partial source success substitutes for these gates.
