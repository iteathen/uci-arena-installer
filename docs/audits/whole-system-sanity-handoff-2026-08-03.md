# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release conclusion exists**  
Snapshot time: 2026-08-03 16:55 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`

This is the canonical continuation state for the six-repository sanity audit. It supersedes earlier same-day handoff snapshots while preserving their commits, issue bodies, PRs, and review comments as immutable evidence. A new agent must be able to identify authoritative ancestry, accepted and rejected work, temporary Jules state, validation performed, validation still required, and the exact next action without conversation memory.

## 1. Governing authority and method

Read, in order:

1. `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`
2. `docs/audits/whole-system-sanity-2026-08.md`
3. `docs/audits/whole-system-performance-addendum-2026-08-03.md`
4. `docs/audits/whole-system-branch-topology-addendum-2026-08-03.md`
5. this handoff and its YAML companion
6. `docs/audits/jules-workbenches/registry.yaml`
7. each touched repository's `AGENTS.md`, read-first spec, active spec registry, issue body, and review comments named below

Non-negotiable rules:

- project-owned neutral LEGO contracts at repository and component boundaries;
- one authority per concern; Manager is not Installer and providers own private domains;
- branch ancestry is product correctness;
- immutable artifacts, manifests, and receipts replace peer-source imports and hidden discovery;
- no filesystem, JSON, logging, blocking locks, repeated transformations, avoidable allocation/copy, or per-element dispatch in hot paths;
- domain-appropriate exact types, ranges, schemas, identities, and lifecycle authority;
- exact evidence before claims; PR prose and aggregate test counts are not proof;
- Jules output is hostile input until its authored delta, omissions, call sites, tests, races, cleanup, and performance are reviewed;
- repairs are ownership-sized, not arbitrary patches or broad cross-owner rewrites;
- source review cannot establish release readiness without native, packaged, destructive, recovery, and performance evidence.

Automatic CI remains disabled by project policy. Required validation is local/manual and must be recorded exactly.

## 2. Environment and validation limits

The connected GitHub capability can inspect and modify the private repositories, create branches/files/issues/commits/PRs, and compare ancestry. It cannot create a new GitHub repository, authorize Jules, or provide the complete private checkout/toolchain environment.

Available local execution during this continuation:

- Linux C++20 compiler with strict warnings;
- Node 22.16 for isolated pure JavaScript semantic tests;
- no repository-required Node 24.15 runtime;
- no Windows SDK/SCM host;
- no complete Linux systemd host;
- no complete private repository checkout or GitHub CLI authentication;
- no real ONNX-linked Engine, installed-suite, or complete Installer lifecycle environment.

No final evidence currently exists for:

- complete Windows/Linux Engine, Manager, or Installer configure/build/CTest on final ancestry;
- Node >=24.15 complete Evidence, Book Forge, or Lichess installed-package suites;
- current real ONNX parity, cancellation, reload, and composition;
- representative one-thread/multicore useful-search benchmarks;
- complete clean install → use → repair → upgrade → reinstall → recovery → uninstall → purge.

Focused source and semantic evidence remain useful but are not native or release evidence.

## 3. Current conclusion

The audit is **not complete**. Critical trust, exact-file, storage, lifecycle, inference, search-concurrency, provider, Installer transaction, uninstall, and packaged end-to-end findings remain open.

`PKG-E2E-011` remains unknown. No soundness conclusion and no release-readiness conclusion may be issued.

## 4. Repository state

### 4.1 Installer — suite composition and deployment authority

Authoritative base: PR #2, `agent/release-assembly`.

Reviewed stack:

- PR #3 bounded indexed-asset results → #2; native Windows/Linux integration still required.
- PR #4 native process-tree stop → #2; Windows Job Object and Linux native evidence still required.
- PR #5 typed service-control actions → #4.
- **PR #11 platform-neutral service-control results → #5**, draft and mergeable.

Installer PR #11 details:

- branch: `agent/platform-neutral-service-control-result`;
- base: `agent/type-service-control-actions` / PR #5;
- head SHA: `58da2deac5a8952ed844471fdc08ac85b53935a0`;
- zero commits behind PR #5; exactly six ownership files changed;
- replaces ambiguous `int` results with `ServiceActionResult` containing action, completed/failed outcome, neutral state, bounded operator-only platform code, and bounded sanitized diagnostic;
- Linux performs bounded `systemctl` execution, exact `ActiveState` query, inactive/query-failure distinction, and final-state validation;
- Windows preserves SCM wait hints/deadline, closes handles through RAII, and maps OS failures/states into the same result;
- new governing spec: `specs/service_control_result_v1.yaml`.

Validation performed for PR #11:

- exact Linux adapter/header compiled under C++20 with `-Wall -Wextra -Wpedantic -Werror`;
- pure mapping/result probe passed;
- missing/unqueryable service probe returned typed `failed / unknown`, platform code `1`, bounded diagnostic;
- Windows compile and real SCM integration remain required.

Open critical issues:

- #7 exact stable-byte TUF/catalog/state admission;
- #8 bounded acquisition/cache/archive expansion;
- #9 truthful transactional apply/rollback/crash recovery;
- #10 exact identity-bound uninstall and purge.

Issues #7–#10 block `PKG-E2E-011` and all lifecycle conclusions.

### 4.2 Engine — chess/search/evaluation/model/inference/runtime authority

Authoritative stack:

1. PR #39 `agent/system-sanity`
2. PR #44 `agent/model-adapter-contract` → #39
3. PR #47 `agent/integrate-engine-audit-stacks` → #44

Reviewable child: PR #52 canonical FEN admission → #47; full Engine gates remain.

Inference remains open:

- issue #57 is the preferred first provider-free contract slice;
- issue #45 remains the broader neutral inference plus ONNX delegation task;
- issue #46 composition root remains blocked;
- Jules PRs #60 and #61 were hostile-reviewed, rejected, and closed;
- PR #61 review comment `5172691145` records forbidden `void*`, hot-path vectors, incomplete identity/cancellation/error contracts, unsafe concurrent provider objects, and relocated model/chess semantics without parity or performance proof.

Transposition-table issues #43/#54 remain open; PRs #51/#55 remain rejected. Performance issues #56/#58/#59 remain open.

Shared-runtime prerequisite:

- issue #62: **Expose timestamped typed foreground revocation from the shared resource lease**;
- source branch `agent/timestamp-resource-lease-revocation` is byte-identical to PR #47 head;
- exact authoritative source SHA: `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`;
- consumer: Evidence issue #21;
- direct source-repository Jules label is forbidden;
- workbench package is prepared and registered, but the private workbench repository has not been created.

Next Engine order:

1. create and authorize the registered private issue #62 workbench;
2. seed its `main` from exact PR #47 SHA and record the workbench baseline commit;
3. launch Jules only in that workbench;
4. transfer and hostile-review the issue #62 delta in the source repository;
5. implement/review issue #57 provider-free inference contract;
6. implement the small ONNX adapter preserving exact model/adapter/tuning identity and safe concurrency;
7. implement issue #46 composition root;
8. implement and benchmark production TT.

### 4.3 Manager — installed-suite control-plane authority

Authoritative base: PR #54, `agent/release-assembly`.

Reviewable child: PR #73 bounded match-plan admission → #54; complete native suites remain.

Blocked storage child: PR #67 — **do not merge**. Review comment `4848995624` records coercive storage projection, unsafe ancestor creation, unbound database/WAL/SHM identity, schema mutation before classification, missing verified backup, incomplete migration/integrity authority, and unproven deterministic failure recovery.

LocalAuth issue #69 remains open. Jules PR #77 was rejected and closed; comment `5172704927` records unsafe path/key admission, nonexclusive/nondurable publication, no Windows ACL authority, ambiguous cleanup failure after publication, and cached bearer authority surviving malformed/unsafe rotation.

UCI issues #70/#76 remain open. Issue #76 is the preferred pure identity/option/override contract before broad process/path integration. Jules PR #74 was rejected and closed.

Next Manager order:

1. reconstruct #69 on exact PR #54 ancestry, using a later private workbench only after the current Engine workbench transaction is deleted;
2. implement #76 before #70;
3. reconstruct PR #67 as one exact storage lifecycle rather than patching it;
4. keep PR #73 draft until full native evidence.

### 4.4 Evidence Service — evidence scheduling/publication authority

Authoritative stack:

1. PR #10
2. PR #13 strict CLI grammar
3. PR #14 streaming export
4. PR #15 streaming display
5. PR #16 transactional crash-recoverable prune
6. PR #17 orphan-boundary integration
7. PR #18 exact config/pointer/owner-root admission
8. PR #19 immutable daemon timing
9. PR #22 resource configuration type/cardinality bounds

**Issue #20 is implemented in draft PR #23**:

- branch: `agent/project-daemon-heartbeat-staleness`;
- base: PR #22 branch `agent/harden-resource-config-after-daemon-timing`;
- head SHA: `3991983c397a222871cba70c6232da1a8be5b265`;
- zero commits behind PR #22; exactly six files changed; draft and mergeable;
- pure `src/daemon_liveness.mjs` owns timestamp parsing, future clamping, age, and fixed 300000 ms threshold;
- public fields: `heartbeat_state` (`missing|fresh|stale`) and nullable `heartbeat_age_ms`;
- existing `heartbeatAt` is preserved;
- direct CLI init/status and Manager CLI-JSON status consume one immutable projector;
- liveness remains observational and does not imply lock/PID/process ownership;
- new governing spec: `specs/daemon_liveness_v1.yaml`.

Validation performed for PR #23:

- isolated semantic suite under available Node 22.16: 5 passed, 0 failed;
- exact 299999/300000 boundaries, missing/malformed, future timestamp, phase independence, immutability, compatibility, invalid clock/threshold;
- Node >=24.15 complete suite remains required.

Issue #21 foreground revocation deadline:

- prepared branch `agent/end-to-end-foreground-revocation`, based on PR #22 ancestry;
- canonical review comment `5172787455`;
- blocked on Engine issue #62;
- current loop can spend almost one second waiting for the next poll, await resource probing, then start a fresh two-second shutdown allowance, and release the cooperative lease only after later cleanup;
- required design uses earliest request visibility time, one absolute 2000 ms deadline, remaining-budget tree shutdown, verified descendant termination, and immediate lease release before guard/catalog/status cleanup.

All Evidence work still requires Node >=24.15, `npm run check`, `npm run acceptance:idle`, package-installed Windows/Linux behavior, and destructive recovery evidence.

### 4.5 Book Forge — opening-book production authority

PRs #8/#11/#10 remain blocked as acceptance evidence.

Issue #12 exact acquisition credential bytes:

- canonical expanded comment `5172735375`;
- current `stat` then `readFile` can read different objects, decodes/materializes before enforcing the byte limit, and `trim()` silently changes credential authority;
- existing tests incorrectly place credentials inside the workspace after validation and accept terminal newlines; reverse those fixtures;
- required owner performs exact descriptor/handle admission at use time, physical ownership proof, 4 KiB plus one-byte overflow detection, exact byte grammar, callback-scoped use, no cache, and generic redacted diagnostics.

Issue #13 recoverable snapshot compilation/publication:

- canonical expanded comment `5172750056`;
- required one `SnapshotBuildOperation` owns lease, private workspace, one selected-row fanout pass, one independent stable-descriptor validation pass, versioned receipt, filesystem/catalog/channel/frontier reconciliation, and bounded cleanup evidence.

Issue #14 catalog and 64-shard SQLite authority:

- canonical expanded comment `5172758661`;
- required local native SQLite/VFS or isolated storage owner, exact schema/application/version identity, BigInt/exact storage-class translation, verified backup/migration/recovery, and catalog-backed cross-shard operation receipts;
- existing tests that normalize partial versioned schemas must be reversed.

Reopened ledger rows remain `BOOK-TRUST-017`, `BOOK-AUTH-018`, and `BOOK-SCALE-020`.

### 4.6 Lichess Bot — Lichess provider authority

PR #3 remains blocked; aggregate passing counts are not acceptance evidence.

Canonical issue comments:

- #4 exact credential/profile/engine/runtime file owner — `5172632661`;
- #5 byte UCI framing, typed options, exact final applied identity — `5172640649`;
- #6 transactional game activation and off-loop serialized status — `5172653544`;
- #7 exact SQLite foundation — `5172666890`.

Required ownership order:

1. exact-file/platform identity and durable publication;
2. raw-byte UCI framing plus one immutable qualification identity shared by capacity/smoke/timing/provenance;
3. durable activation nonce/phase, exact slot correlation, structured terminal cleanup, latest-wins off-loop status publisher;
4. exact database/WAL/SHM owner, typed translation, schema/migration/recovery.

Do not merge PR #3 or promote a provider candidate until #4–#7 pass real-process, loopback API, storage, startup, active-game latency, package, race, and recovery evidence.

## 5. Jules trust and disposable-workbench protocol

Observed behavior: issue-label Jules tasks started from repository default `main` even when issue text named prepared branches. Intermediate Jules error comments are not terminal. Completed PRs remain untrusted until the authored commit is reviewed.

Rejected/closed outputs include Engine #49/#50/#51/#55/#60/#61 and Manager #72/#74/#77.

A workbench is a temporary transaction, not a development repository:

`created → baseline_verified → jules_active → result_frozen → transferred → equivalence_verified → destination_pr_open → deleted`

Rules:

1. create one private `iteathen/jules-wb-<project>-<issue>-<attempt>` repository and authorize Jules;
2. flatten only exact accepted prerequisite ancestry into workbench `main`;
3. commit a baseline manifest with source repo/branch/SHA, destination branch, task, permitted owners/files, and cleanup state;
4. run one task only and never reuse the repository;
5. freeze result SHA and extract only `baseline..result`;
6. transfer accepted delta to a fresh branch from original authoritative ancestry;
7. verify patch equivalence and hostile-review in the original repository;
8. record SHAs/PR/transcript, delete workbench repository and local clones/worktrees, and mark state `deleted`.

Central registry: `docs/audits/jules-workbenches/registry.yaml`.

Registered transaction:

- id: `engine-62-20260803`;
- intended private repository: `iteathen/jules-wb-engine-62-20260803`;
- state: `awaiting_repository_creation`;
- source: `iteathen/UCI_Arena`, PR #47 branch, SHA `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`;
- destination: `agent/timestamp-resource-lease-revocation`;
- source issue: #62;
- manifest: `docs/audits/jules-workbenches/engine-62-20260803.yaml`;
- task body: `docs/audits/jules-workbenches/engine-62-20260803-task.md`;
- no Jules task ID, result SHA, transfer SHA, or destination PR exists yet;
- no `jules-wb-*` repository is visible through the connected installation;
- connector cannot create the private repository or authorize Jules;
- no second workbench transaction should be started until this one is transferred or abandoned and deleted.

## 6. Immediate restart order

1. Read this handoff, YAML, workbench registry, architecture, ledger, performance, and topology documents.
2. Fetch current heads before trusting recorded SHAs.
3. Review Evidence PR #23 and Installer PR #11 on their exact stacked bases; run required native/Node suites when environments exist.
4. User creates private `iteathen/jules-wb-engine-62-20260803`, authorizes Jules, and seeds `main` from exact Engine SHA `60f7bc9f...`.
5. Commit the workbench manifest/task into that repository, record the resulting baseline SHA in the registry, then apply the `jules` label there.
6. Freeze, transfer, equivalence-check, hostile-review, and delete the Engine #62 workbench before creating another.
7. Implement Evidence #21 after accepted #62 code is vendored exactly.
8. Continue direct ownership-sized repairs where safe: Manager #76, Book Forge #12, or another small pure contract.
9. Use later workbenches for large cross-platform exact-file/SQLite/lifecycle reconstructions only after the active transaction is cleaned up.
10. Resolve Installer #7–#10 before any package/lifecycle conclusion.
11. Converge ledger statuses only after accepted fixes enter authoritative ancestry.
12. Before stopping, update both handoff files, workbench registry, exact validation, and the discoverability comment on Installer PR #2.

## 7. Final gates before any soundness decision

Source/architecture:

- all active specs, preflight, and boundary checks pass;
- one authoritative ancestry per repository with no orphan fixes;
- no copied ownership or peer-source imports;
- all substitution probes pass;
- all reopened ledger rows are reclassified with concrete evidence.

Native/runtime:

- Windows/Linux Engine, Manager, and Installer configure/build/CTest;
- Node >=24.15 complete Evidence, Book Forge, and Lichess suites;
- complete Lichess loopback, real-process, storage, startup, and active-game tests;
- real ONNX parity/cancellation/reload/composition;
- TT race/memory and one-thread/multicore useful-search benchmarks;
- storage migration/corruption/race/recovery on both platforms.

Package/lifecycle:

- immutable candidates from accepted ancestry;
- exact manifests, receipts, inventories, digests, SBOMs, provenance, and dependency closure;
- Windows personal/all-users and Linux package behavior;
- clean install, ordinary use, repair, upgrade, reinstall;
- injected crash/restart recovery;
- normal uninstall preservation and safe reinstall revalidation;
- explicit previewed purge with exact ownership/link safety;
- repeated cycle without stale startup, receipt, lock, cache, transaction, or workbench debris.

No partial source-level success substitutes for these gates.

## 8. Maintenance rule

Update this file and its YAML companion whenever authoritative ancestry, a finding, Jules task/result, workbench state, PR/issue disposition, validation result, or next restart action changes.

The handoff is incomplete if a new agent would need conversation memory to determine what is authoritative, rejected, tested, temporary, or next.
