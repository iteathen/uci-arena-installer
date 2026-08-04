# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release conclusion exists**  
Snapshot time: 2026-08-03 17:10 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`

This is the canonical continuation state for the six-repository sanity audit. It supersedes earlier same-day snapshots while preserving their commits, issue bodies, PRs, and review comments as immutable evidence. A new agent must be able to identify authoritative ancestry, accepted and rejected work, temporary Jules state, validation performed, validation still required, and the exact next action without conversation memory.

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
- no complete private repository checkout or authenticated GitHub CLI;
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

Installer PR #11:

- branch `agent/platform-neutral-service-control-result`;
- head `58da2deac5a8952ed844471fdc08ac85b53935a0`;
- zero commits behind PR #5; exactly six ownership files;
- replaces ambiguous `int` results with `ServiceActionResult` containing action, completed/failed outcome, neutral state, bounded operator-only platform code, and bounded sanitized diagnostic;
- Linux performs bounded `systemctl` execution, exact `ActiveState` query, inactive/query-failure distinction, and final-state validation;
- Windows preserves SCM wait hints/deadline, closes handles through RAII, and maps OS failures/states into the same result;
- governing spec `specs/service_control_result_v1.yaml`.

Validation performed for PR #11:

- exact Linux adapter/header compiled under C++20 with `-Wall -Wextra -Wpedantic -Werror`;
- pure mapping/result probe passed;
- missing/unqueryable service probe returned typed `failed / unknown`, platform code `1`, bounded diagnostic;
- Windows compile and real SCM/systemd integration remain required.

Open critical issues #7–#10 remain package/lifecycle blockers:

- #7 exact stable-byte TUF/catalog/state admission;
- #8 bounded acquisition/cache/archive expansion;
- #9 truthful transactional apply/rollback/crash recovery;
- #10 exact identity-bound uninstall and purge.

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

- issue #62, timestamped typed foreground revocation;
- source branch `agent/timestamp-resource-lease-revocation` is byte-identical to PR #47 head;
- exact source SHA `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`;
- consumer Evidence issue #21;
- direct source-repository Jules label forbidden;
- registered workbench awaits account-side repository creation.

### 4.3 Manager — installed-suite control-plane authority

Authoritative base: PR #54, `agent/release-assembly`, head baseline `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

Reviewable work:

- PR #73 bounded match-plan admission → #54; native suites required.
- **PR #78 pure UCI identity and option contract → #54**, draft and mergeable.

Manager PR #78:

- branch `agent/uci-option-contract-v1`;
- head `a6420cf26a2b347e611610b46a6795d89b28c77f`;
- zero commits behind PR #54; exactly six changed files; CMake changes exactly two registrations;
- strict UTF-8 scalar admission, identity cardinality, option grammar, metadata bounds, ASCII-case-fold uniqueness, typed override validation, and deterministic canonical option spelling;
- exact button/check/spin/combo/string contracts;
- no process, path, executable hash, receipt, command, UI, or Installer ownership;
- governing spec `specs/uci_option_contract_v1.yaml`;
- issue #70 remains open for bounded process framing, launch/path identity, executable binding, and integration into `UciProcess`.

Validation performed for PR #78:

- production parser compiled under strict C++20 warnings against the exact public/domain shape;
- valid all-type and canonical override probes passed;
- adversarial probe passed duplicate identity, missing identity, malformed UTF-8, strict integer, duplicate combo variant, button override, and 256-option cases;
- representative 256-button parse measured 522 allocations on available Linux libstdc++;
- full Manager Linux/Windows CMake/Catch2 suites remain required.

Blocked work:

- PR #67 storage — do not merge; review `4848995624` requires one exact database/WAL/SHM/schema/backup/migration/recovery lifecycle.
- LocalAuth issue #69 remains open; Jules PR #77 rejected, review `5172704927`.
- UCI issue #70 remains open after PR #78 and should consume the accepted pure contract rather than duplicate it.

### 4.4 Evidence Service — evidence scheduling/publication authority

Authoritative stack: PRs #10, #13, #14, #15, #16, #17, #18, #19, and #22.

**PR #23 is a partial issue #20 projector slice, not an issue-closing fix**:

- title `Add the Evidence daemon heartbeat liveness projector`;
- branch `agent/project-daemon-heartbeat-staleness`;
- base PR #22; head `3991983c397a222871cba70c6232da1a8be5b265`;
- draft, mergeable, zero behind, exactly six changed files;
- pure `src/daemon_liveness.mjs` owns timestamp parsing, future clamping, age, and fixed 300000 ms threshold;
- fields `heartbeat_state` (`missing|fresh|stale`) and nullable `heartbeat_age_ms`;
- CLI and Manager API wrappers consume one immutable projector;
- isolated Node 22 suite: 5 passed, 0 failed.

Remaining before issue #20 can close:

- `Catalog.status()` itself must import and own the projection so every direct catalog consumer receives the typed fields;
- capture one status-sample clock and use it for liveness and `sampledAt`;
- remove redundant CLI/API wrappers after catalog ownership exists;
- add a real temporary-catalog test for direct status boundaries and Manager compatibility;
- run Node >=24.15 full checks.

The connected environment cannot apply a narrow patch to the 750-line catalog file and has no private checkout/archive capability; replacing that entire file through the contents API was rejected as unnecessary rewrite risk. PR #23 body records this limitation and issue #20 remains open.

Issue #21 foreground revocation:

- prepared branch `agent/end-to-end-foreground-revocation` on PR #22;
- blocked on Engine #62;
- requires earliest request visibility, one absolute 2000 ms deadline, remaining-budget tree shutdown, verified descendant termination, and immediate cooperative lease release before later cleanup.

### 4.5 Book Forge — opening-book production authority

PRs #8/#11/#10 remain blocked as acceptance evidence.

- Issue #12 exact acquisition credential bytes, expanded comment `5172735375`: exact descriptor/handle admission at use time, physical ownership reproof, 4 KiB plus one overflow detection, exact byte grammar without `trim()`, callback-scoped use, no cache, generic redacted diagnostics; reverse tests that place credentials inside workspace or accept terminal newline.
- Issue #13 recoverable snapshot transaction, comment `5172750056`: one operation owns lease, private workspace, one selected-row fanout, one independent stable-descriptor validation, receipt, activation/catalog/channel/frontier reconciliation, and cleanup evidence.
- Issue #14 catalog/64-shard SQLite authority, comment `5172758661`: native SQLite/VFS or isolated owner, exact path/WAL/SHM/schema/version/storage-class authority, verified backup/migration/recovery, cross-shard operation receipts; reverse partial-schema normalization tests.

Reopened ledger rows: `BOOK-TRUST-017`, `BOOK-AUTH-018`, `BOOK-SCALE-020`.

### 4.6 Lichess Bot — Lichess provider authority

PR #3 remains blocked; aggregate passing counts are not acceptance evidence.

Canonical blockers:

- #4 exact file/publication/launch identity — comment `5172632661`;
- #5 raw-byte UCI, typed options, exact final identity — `5172640649`;
- #6 transactional activation and off-loop latest-wins status — `5172653544`;
- #7 exact SQLite foundation — `5172666890`.

Ownership order remains #4 → #5 → #6 → #7. No provider candidate may be promoted until real-process, loopback API, storage, startup, active-game latency, package, race, and recovery evidence pass.

## 5. Jules trust and disposable-workbench protocol

Issue-label Jules tasks started from repository default `main` even when issue text named prepared branches. Intermediate error comments are not terminal. Completed outputs remain untrusted until the authored delta is reviewed.

Rejected/closed outputs include Engine #49/#50/#51/#55/#60/#61 and Manager #72/#74/#77.

Workbench states:

`created → baseline_verified → jules_active → result_frozen → transferred → equivalence_verified → destination_pr_open → deleted`

Protocol:

1. create one private `iteathen/jules-wb-<project>-<issue>-<attempt>` repo and authorize Jules;
2. flatten only exact accepted prerequisite ancestry into workbench `main`;
3. commit source/destination/scope/cleanup manifest and task;
4. run one task only; never reuse the repo;
5. freeze result SHA and extract only `baseline..result`;
6. transfer to fresh source branch from authoritative ancestry;
7. verify patch equivalence and hostile-review in source repo;
8. record SHAs/PR/transcript; delete workbench and local clones/worktrees; mark deleted.

Central registry: `docs/audits/jules-workbenches/registry.yaml`.

Registered transaction:

- id `engine-62-20260803`;
- intended repo `iteathen/jules-wb-engine-62-20260803`;
- state `awaiting_repository_creation`;
- source Engine PR #47 SHA `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`;
- destination `agent/timestamp-resource-lease-revocation`;
- issue #62;
- manifest `docs/audits/jules-workbenches/engine-62-20260803.yaml`;
- task `docs/audits/jules-workbenches/engine-62-20260803-task.md`;
- no task/result/transfer/destination PR exists;
- no second workbench may begin until this transaction is transferred or abandoned and deleted.

## 6. Immediate restart order

1. Read this handoff, YAML, registry, architecture, ledger, performance, and topology files.
2. Fetch current heads before trusting recorded SHAs.
3. Review and run native/full suites for Installer PR #11 and Manager PR #78.
4. Complete `Catalog.status()` ownership for Evidence PR #23 using a real checkout; do not claim issue #20 closed before that.
5. User creates/authorizes `iteathen/jules-wb-engine-62-20260803`, seeds exact Engine SHA, commits manifest/task, records baseline SHA, and applies `jules` only there.
6. Freeze, transfer, equivalence-check, hostile-review, and delete the workbench before another workbench is prepared.
7. Implement Evidence #21 after accepted #62 code is vendored exactly.
8. Continue direct ownership-sized repairs where coherent; use later workbenches for large cross-platform exact-file/SQLite/lifecycle reconstructions only after the active transaction is cleaned up.
9. Resolve Installer #7–#10 before any package/lifecycle conclusion.
10. Update both handoffs, registry, validation, and Installer PR #2 comment before stopping.

## 7. Final gates before any soundness decision

Source/architecture:

- all active specs, preflight, and boundary checks;
- one authoritative ancestry per repository;
- no orphan fixes, copied ownership, or peer-source imports;
- all substitution probes;
- all reopened ledger rows reclassified with concrete evidence.

Native/runtime:

- Windows/Linux Engine, Manager, and Installer build/CTest;
- Node >=24.15 complete Evidence, Book Forge, and Lichess suites;
- complete Lichess loopback/process/storage/startup/active-game tests;
- real ONNX parity/cancellation/reload/composition;
- TT race/memory and one-thread/multicore useful-search benchmarks;
- storage migration/corruption/race/recovery on both platforms.

Package/lifecycle:

- immutable candidates from accepted ancestry;
- exact manifests, receipts, inventories, digests, SBOMs, provenance, dependency closure;
- Windows personal/all-users and Linux package behavior;
- clean install/use/repair/upgrade/reinstall;
- injected crash/restart recovery;
- uninstall preservation and safe reinstall;
- previewed exact-identity purge;
- repeated cycle without stale startup, receipt, lock, cache, transaction, install, or workbench debris.

No partial source-level success substitutes for these gates.

## 8. Maintenance rule

Update this file and its YAML companion whenever ancestry, finding, Jules/workbench state, PR/issue disposition, validation, or next action changes. The handoff is incomplete if a new agent would need conversation memory to determine what is authoritative, rejected, tested, temporary, or next.
