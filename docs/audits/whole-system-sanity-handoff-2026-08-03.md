# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release conclusion exists**  
Snapshot time: 2026-08-03 16:10 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`

This file is the canonical continuation state for the six-repository audit. It supersedes the 15:00 snapshot committed as `d55ebc4` / `6722974` while preserving those commits as immutable historical evidence. The detailed ledger, architecture, performance addendum, topology addendum, issue bodies, and hostile-review comments remain authoritative evidence for their ownership slices.

## 1. Read-first authority and method

Read, in order:

1. `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`
2. `docs/audits/whole-system-sanity-2026-08.md`
3. `docs/audits/whole-system-performance-addendum-2026-08-03.md`
4. `docs/audits/whole-system-branch-topology-addendum-2026-08-03.md`
5. this handoff and its YAML companion
6. each touched repository's `AGENTS.md`, read-first specification, active spec registry, and ownership specification
7. the issue and hostile-review comments named below

Non-negotiable rules:

- **LEGO boundaries:** project-owned neutral contracts at repository/component boundaries; concrete adapters behind ports; composition owns construction.
- **One owner per concern:** Manager is not Installer; providers own their private domains; Engine owns chess/search/evaluation/model/inference/runtime tools.
- **Branch ancestry is product correctness:** a fix outside the candidate ancestry is absent from the product.
- **No source-tree coupling or hidden discovery:** repositories exchange immutable artifacts, manifests, receipts, typed versioned contracts, and bounded locators.
- **Hot-path discipline:** no JSON, filesystem, logging, blocking locks, repeated transformations, avoidable allocation/copy, or per-element virtual dispatch in node/tensor/record loops.
- **Domain-appropriate foundations:** exact types, identities, ranges, schemas, and lifecycle authority must survive the real domain.
- **Evidence before claims:** exact commands/output are required; aggregate counts and PR prose are not proof.
- **Jules output is untrusted:** inspect authored commits, omissions, call sites, tests, races, cleanup, and performance; reconstruct accepted work on authoritative ancestry.
- **Fix by ownership boundary:** coherent owner-sized changes, not arbitrary small patches or broad cross-owner rewrites.
- **No release conclusion from source review:** native, packaged, installed, destructive, recovery, and performance gates must be capable of disproving the source theory.

Automatic CI remains disabled by project policy. Validation is local/manual and must be recorded exactly.

## 2. Environment limits

The connected GitHub tool can inspect and modify the private repositories, but the current execution environment does not provide the complete private checkouts, Node 24.15 runtime, Windows/Linux native toolchains, installed provider payloads, real ONNX-linked Engine build, or complete installer lifecycle environment.

Therefore:

- connector review and focused source evidence are useful but are not native/package evidence;
- no complete Windows/Linux Engine, Manager, or Installer build/CTest has been established for the final ancestry;
- no final Node >=24.15 Evidence, Book Forge, or Lichess package-installed suite has been established;
- no representative current Engine useful-search benchmark has been completed;
- no complete clean install → use → repair → upgrade → reinstall → recovery → uninstall → purge cycle has been completed.

## 3. Overall conclusion

The sanity check is **not complete**. Critical trust, exact-file, storage, lifecycle, inference, search-concurrency, provider, installer transaction, uninstall, and packaged end-to-end findings remain open.

`PKG-E2E-011` remains unknown. No soundness conclusion and no release-readiness conclusion may be issued.

## 4. Canonical repository state

### 4.1 Installer — suite composition and deployment authority

Authoritative base:

- PR #2 — `agent/release-assembly` — canonical release assembly, audit ledger, and this handoff.

Reviewed children:

- PR #3 — bounded indexed-asset results → base #2; native Windows/Linux integration still required.
- PR #4 — native process-tree stop → base #2; Windows Job Object and Linux CMake/CTest still required.
- PR #5 — typed service-control actions → base #4; result semantics remain non-substitutable.

Open critical issues:

- #6 platform-neutral service-control result;
- #7 exact stable-byte TUF/catalog/state admission;
- #8 bounded acquisition/cache/archive expansion;
- #9 truthful transactional apply/rollback/crash recovery;
- #10 exact identity-bound uninstall and purge.

Issues #7–#10 block `PKG-E2E-011` and all lifecycle conclusions.

### 4.2 Engine — chess/search/evaluation/model/inference/runtime authority

Authoritative stack:

1. PR #39 — `agent/system-sanity`
2. PR #44 — `agent/model-adapter-contract` → #39
3. PR #47 — `agent/integrate-engine-audit-stacks` → #44

Reviewable child:

- PR #52 — canonical FEN admission → #47; static/standalone evidence exists, full Engine gates remain.

Inference:

- Issue #45 remains open.
- Issue #57 remains open as the preferred first provider-free contract slice.
- Jules PR #60 (task `12435499143853197163`) was rejected and closed: wrong ancestry plus unsound contract/probe semantics.
- Jules PR #61 (task `11856418714977393425`) was hostile-reviewed, rejected, and closed. Its neutral header used forbidden `void*`, hot-path vectors, unbounded identities, no typed per-call failure/cancellation/stale-completion authority, and omitted model-graph identity. Its 2,562-line ONNX source relocated model/chess semantics, cleared `input_adapter_id_`, lacked safe concurrent binding ownership and provider cancellation, and did not preserve the required validation/performance evidence. See PR #61 comment `5172691145`.
- Issue #46 composition root remains blocked until the neutral contract and concrete inference adapter are accepted.

Transposition table and performance:

- Issues #43/#54 remain open; no accepted production lock-free TT exists.
- PRs #51 and #55 remain rejected source evidence.
- Issues #56, #58, and #59 remain open for measured machine-code hot paths, killer/history synchronization, and incremental MCGS repetition history.

Next Engine order:

1. construct a disposable branch-correct workbench or work directly on exact PR #47 ancestry;
2. implement/review the small provider-free contract with fixed bounded views, explicit capacities, checked products, typed per-call cancellation/result semantics, and complete probe evidence;
3. implement a small concrete ONNX adapter preserving model/tuning identity and safe concurrency;
4. implement composition root #46;
5. implement/benchmark production TT;
6. run full correctness, parity, cancellation, reload, CTest, and useful-search performance gates.

### 4.3 Manager — installed-suite control-plane authority

Authoritative base:

- PR #54 — `agent/release-assembly`.

Reviewable child:

- PR #73 — bounded match-plan admission → #54; focused compiler/sanitizer semantics passed, complete Windows/Linux suites remain.

Blocked storage child:

- PR #67 — SQLite foundation → #54 — **do not merge**.
- Existing findings remain: coercive storage-class projection, unsafe ancestor creation, incomplete schema/migration authority, missing adversarial tests.
- Continued review added: no verified backup before migration; inspected paths not bound to opened database/WAL/SHM identities; coercive pragma/text reads; no exact singleton migration or schema contract; `CREATE IF NOT EXISTS` mutates before classification; no application/integrity admission; rollback/close/retry evidence absent. Review comment ID `4848995624`.

LocalAuth:

- Issue #69 remains open.
- Jules PR #77 (task `15207249830771102373`) was hostile-reviewed, rejected, and closed. It was main-based and also changed unrelated UCI owners. Its LocalAuth slice created directories before exact ancestor proof, used check-then-open unbounded key reads, path-based permissions without Windows ACL proof, nonexclusive/non-durable temporary publication, ambiguous success after cleanup failure, and cached old bearer authority before validating malformed/unsafe rotation. Tests did not establish the required platform/race/durability matrix. See PR #77 comment `5172704927`.

UCI admission:

- Issue #70 remains open.
- Jules PR #74 (task `5783702717709122040`) was rejected/closed. It was main-based and repository-wide; the UCI slice accepted duplicate identity, lacked exact per-type option grammar, bounded output acquisition, exact launch-byte identity, and the required adversarial/native evidence.
- Issue #76 remains the preferred small pure UCI identity/option/override contract and should precede broad subprocess/path integration.

Manager next order:

1. reconstruct LocalAuth #69 on exact #54 ancestry, preferably through the disposable workbench protocol;
2. establish pure UCI contract #76, then integrate #70 around it;
3. reconstruct PR #67's storage owner with exact path/open identity, typed projection, schema classification, verified backup, transactional migration, integrity/pragma admission, and deterministic recovery;
4. retain PR #73 as draft until complete native tests;
5. run spec checks, Windows/Linux build/CTest, and installed authentication/probe/tournament traces.

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

Open blockers:

- #20 stale-heartbeat status projection;
- #21 true end-to-end foreground revocation budget.

All later Evidence PRs still require Node >=24.15, `npm run check`, `npm run acceptance:idle`, package-installed Windows/Linux behavior, and destructive recovery evidence. Node 22 probes are not release evidence.

### 4.5 Book Forge — opening-book production authority

Documented stack:

1. PR #8
2. PR #11 → #8
3. PR #10 → #11

All remain blocked as acceptance evidence.

Open critical issues:

- #12 exact acquisition credential bytes at use time;
- #13 single-pass-enough recoverable snapshot compilation/publication;
- #14 exact catalog and 64-shard SQLite authority.

Reopened ledger rows remain `BOOK-TRUST-017`, `BOOK-AUTH-018`, and `BOOK-SCALE-020`. Aggregate 59/59 and deterministic candidate claims do not close them.

### 4.6 Lichess Bot — Lichess provider authority

- PR #3 — `agent/system-sanity` — mergeable draft but **blocked by adversarial review**. Its 148-pass aggregate is not acceptance evidence.

Open issues and expanded canonical comments:

- #4 exact credential/profile/engine/runtime files — comment `5172632661`.
  - Exact-file authority is fragmented across token/config/profile/evidence/executable/argument/model/tablebase/status/log/publication paths and both startup adapters.
  - Required: one provider-local exact-file/publication component, bounded descriptor/handle reads and streaming hashes, exact executable launch identity, exclusive durable publication, native Windows reparse identity and POSIX no-follow/descriptor operations.
- #5 byte UCI framing, typed options, and exact final applied identity — comment `5172640649`.
  - Required: raw-byte framer before UTF-8 decode, exact typed handshake parser, fixed telemetry record, one final option map/SHA including automatic `Move Overhead` and `Ponder`, and one immutable qualification identity shared by capacity/smoke/timing/provenance.
- #6 transactional game activation and nonblocking status — comment `5172653544`.
  - Required: exact challenge/recovery correlation, durable activation nonce/phase record, coherent slot/session/task publication behind a start gate, structured terminal/cleanup outcome, and one off-loop latest-wins serialized status publisher.
- #7 exact SQLite storage foundation — comment `5172666890`.
  - Required: native SQLite/VFS or isolated storage owner capable of exact database/WAL/SHM identity; exact schema/application/version classification; BigInt/exact storage-class translation; verified backup/migration/recovery; owner transactions and destructive platform tests.

Additional confirmed Lichess facts:

- `findPending()` currently falls back to opponent or sole-slot adoption;
- active authority can be exposed before challenge/reservation/session/task/durable game authority;
- session failures can be swallowed while cleanup/status failures replace causality;
- current tests intentionally normalize partial v1/v3 schemas rather than rejecting malformed admitted versions.

Reopened ledger rows remain `LICHESS-UCI-012`, `LICHESS-CRED-013`, `LICHESS-LIFECYCLE-014`, plus storage #7. Do not merge PR #3 or promote a candidate until #4–#7 are resolved on final ancestry and exercised with real process, loopback API, storage, race, startup, active-game latency, package, and recovery evidence.

## 5. Jules execution and trust protocol

### Observed behavior

- Intermediate Jules error comments are **not terminal task status**. Both issue #45 and #69 later produced PRs after earlier error/failure-looking comments.
- The GitHub issue-label route has repeatedly produced one-commit branches from repository default `main`, even when the issue text names a prepared branch.
- Wrong ancestry is an integration classification, not proof that the Jules task failed.
- A completed Jules PR is still untrusted until its authored commit is reviewed.

### Rejected/closed outputs

- Engine #49 setup only/no inference implementation.
- Engine #50 stale FEN snapshot; useful work reconstructed as PR #52.
- Engine #51 noncompiling TT and copied test implementation.
- Engine #55 TT planning only.
- Engine #60 unsound provider-free contract/probe.
- Engine #61 unsound broad inference/ONNX relocation.
- Manager #72 empty delivery with false narrative.
- Manager #74 unsound broad UCI reconciliation.
- Manager #77 unsound LocalAuth reconciliation and unrelated UCI changes.

No Jules task is currently treated as active in this handoff. Issue #76 task creation had previously failed; do not infer terminal state from an intermediate bot comment without the Jules UI or later code/PR evidence.

### Mandatory hostile review

For every result:

1. freeze the result SHA;
2. establish the actual merge base and authored commit range;
3. list files changed by the authored commit, not merely the PR comparison against another ancestry;
4. reject unrelated ownership changes;
5. read every changed line and omitted requirement;
6. inspect declarations, aggregation, call sites, error paths, lifetimes, races, cancellation, cleanup, and performance;
7. prove tests call production code and reach the intended assertion;
8. reproduce exact compile/test/benchmark claims or mark them unverified;
9. reconstruct accepted work on authoritative ancestry;
10. keep the destination PR draft until native/platform/package gates pass.

## 6. Disposable Jules workbench protocol

Use this when the issue-label route must start from a stacked authoritative ancestry.

A workbench is a temporary transaction, not a development repository.

Required flow:

1. create one private repository under `iteathen`, explicitly authorized for Jules;
2. flatten only the exact accepted prerequisite ancestry into workbench `main`;
3. commit a baseline manifest recording source repo, source branch/SHA, destination branch, task/issue, permitted files/owners, and cleanup state;
4. verify workbench `main` equals the recorded baseline;
5. create one task/issue only; never reuse a workbench;
6. when Jules produces code, freeze the result SHA and stop mutation;
7. extract only `baseline..result` authored commits/diff;
8. transfer the accepted delta to a fresh branch from the original repository's authoritative destination ancestry;
9. verify patch equivalence and run hostile review in the original repository before opening/advancing the real draft PR;
10. after the original branch/PR, SHAs, equivalence, and transcript are recorded, remove Jules access if separately granted, delete the workbench repository, remove local clones/worktrees, and mark cleanup complete here.

State machine:

`created → baseline_verified → jules_active → result_frozen → transferred → equivalence_verified → destination_pr_open → deleted`

Rules:

- deterministic name: `jules-wb-<project>-<issue>-<attempt>`;
- one task and one source repository per workbench;
- no rejected/superseded/overlapping branch in baseline;
- never merge workbench `main` into the product repository;
- no new workbench while an unlisted or abandoned one exists;
- keep a central registry in this handoff/YAML;
- no workbench currently exists.

## 7. Immediate restart order

1. Read this handoff, YAML, architecture, ledger, performance, and topology files.
2. Fetch current heads before trusting any recorded SHA.
3. Confirm there are no abandoned `jules-wb-*` repositories before creating one.
4. Use a disposable Engine workbench based exactly on PR #47 for the small issue #57 provider-free contract; do not retry the broad #45 monolith first.
5. Use a disposable Manager workbench based exactly on PR #54 for LocalAuth #69, or reconstruct directly with the exact secret-file/platform owner.
6. Implement Manager pure UCI contract #76 before broad #70 integration.
7. Reconstruct Manager SQLite #67 rather than patching its current lifecycle.
8. Continue Lichess #4–#7 in the owner order: exact-file platform owner; UCI byte/identity owner; activation/status owner; SQLite owner. Coordinate shared platform identity without collapsing concerns.
9. Continue Book Forge #12–#14.
10. Implement Evidence #20/#21 on final #22 ancestry and run Node 24.15/package acceptance.
11. Resolve Installer #7–#10 before any package/lifecycle conclusion.
12. Converge ledger statuses only after accepted fixes enter authoritative ancestry.
13. Before stopping, update both handoff files, workbench registry, PR #2 discoverability comment, exact validation, and next action.

## 8. Final gates before any soundness decision

### Source and architecture

- all active specs/preflight/boundary checks pass;
- one authoritative ancestry per repository with no orphan fixes;
- no copied ownership or peer-source imports;
- all substitution probes pass;
- all reopened ledger rows are reclassified with concrete evidence.

### Native and runtime

- Windows/Linux Engine, Manager, and Installer configure/build/CTest;
- Node >=24.15 full suites for Evidence and Book Forge;
- complete Lichess loopback API, real process, storage, startup, and active-game lifecycle tests;
- real ONNX parity/cancellation/reload/composition traces;
- TT correctness/race/memory and one-thread/multicore useful-search benchmarks;
- Evidence/Book preemption and idle acceptance;
- storage reopen/migration/corruption/race/recovery tests on both platforms.

### Package and lifecycle

- immutable provider candidates from accepted ancestry;
- exact manifests, receipts, inventories, digests, SBOMs, provenance, and dependency closure;
- Windows personal/all-users and Linux package behavior;
- clean install and ordinary use across all providers;
- repair, upgrade, reinstall;
- injected crash/restart recovery;
- normal uninstall with preservation proof;
- safe reinstall revalidation;
- explicit previewed purge with exact ownership and link/reparse safety;
- repeated cycle without stale startup, receipt, lock, cache, transaction, or workbench authority.

No partial source-level success substitutes for these gates.

## 9. Handoff maintenance rule

Update this file and its YAML companion whenever any authoritative ancestry, finding, Jules task/result, workbench state, PR/issue disposition, validation result, or next restart action changes.

A handoff is incomplete if a new agent would need conversation memory to determine what is authoritative, what was rejected, what was tested, which temporary repositories exist, or what to do next.
