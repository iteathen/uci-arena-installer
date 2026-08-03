# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release conclusion exists**  
Snapshot time: 2026-08-03 15:00 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`

This document is the continuation handoff for the six-repository sanity audit. It is deliberately redundant with the ledger, branch-topology addendum, performance addendum, issue bodies, and pull-request descriptions so another chat or agent can continue without reconstructing critical decisions from conversation history.

## 1. Governing authority and non-negotiable method

Read these first:

1. `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`
2. `docs/audits/whole-system-sanity-2026-08.md`
3. `docs/audits/whole-system-performance-addendum-2026-08-03.md`
4. `docs/audits/whole-system-branch-topology-addendum-2026-08-03.md`
5. this handoff
6. each touched repository's `AGENTS.md`, read-first specification, active spec registry, and ownership specification

The governing engineering principles are:

- **LEGO boundaries:** project-owned neutral contracts at repository and component boundaries; concrete adapters behind ports; composition owns concrete construction.
- **Strict ownership:** one authority per concern. Manager is not Installer. Providers own their private domains. Engine owns chess/search/evaluation/model/inference and Engine runtime tools.
- **Branch ancestry is product correctness:** a valid fix outside the candidate's ancestry is absent from the product.
- **No source-tree coupling:** repositories exchange immutable artifacts, manifests, receipts, and typed versioned contracts—not peer source imports or private-store inspection.
- **No hidden discovery or product-name dispatch:** discovery is bounded, declarative, receipt/manifest/locator driven, and non-mutating.
- **Hot-path discipline:** no JSON, FEN/string round-trips, filesystem, logging, blocking mutexes, repeated validation, heap allocation, per-element virtual dispatch, or unnecessary full-buffer copies in node/tensor/record loops.
- **Domain-appropriate foundations:** ranges, precision, identities, schemas, and representations must survive the real domain rather than the first observed example.
- **Evidence before claims:** exact commands and output are required. A PR body or aggregate test count is not proof.
- **Jules output is untrusted:** inspect every changed line, ancestry, omission, test, and claim. Reconstruct accepted slices on authoritative ancestry; never merge a Jules snapshot wholesale.
- **Fix by ownership boundary:** coherent owner-sized changes, not small arbitrary batches and not broad cross-owner rewrites.
- **No release conclusion from source review:** packaged execution, native platform behavior, install lifecycle, recovery, and performance must be capable of disproving the source theory.

All automatic CI is intentionally disabled. Required validation is local/manual and must be recorded exactly.

## 2. Environment and validation limitations

The connected GitHub tool can inspect and modify private repositories. The current execution environment does **not** provide a complete private checkout with the required Windows/Linux native toolchains and installed provider payloads.

Known limitations:

- Node available for focused local probes has been 22.16; Evidence and Book Forge require Node >=24.15.0.
- No trustworthy full Windows native build, Linux native build, complete CTest run, installer package construction, or installed-suite lifecycle can be inferred from connector-only work.
- No final Engine ONNX-linked build or representative real search benchmark has been completed for the current audit stack.
- No final complete-suite clean install → use → repair → upgrade → reinstall → uninstall → purge cycle has been completed.

Focused syntax, semantic harness, compiler, sanitizer, and line-review evidence remains useful but must be labelled exactly as such.

## 3. Current overall conclusion

The sanity check is **not complete**.

The architecture and ownership map are substantially understood, many branch-topology defects have been repaired, and several focused owner boundaries now have reviewable draft PRs. However, critical trust, storage, lifecycle, inference, search-concurrency, provider, Installer transaction, uninstall, and packaged end-to-end findings remain open.

`PKG-E2E-011` remains unknown. No soundness conclusion and no release-readiness conclusion may be issued.

## 4. Canonical repository topology and current status

### 4.1 Installer — suite composition and deployment authority

Authoritative base:

- PR #2 — `agent/release-assembly` — independent suite release assembly and whole-system audit ledger.

Reviewed child topology:

- PR #3 — `agent/bound-indexed-asset-results` → base PR #2. Global large-asset result/materialization bound. Mergeable draft; repository-native Windows/Linux index integration still required.
- PR #4 — `agent/harden-native-process-tree-stop` → base PR #2. Process-tree shutdown and descendant proof. Mergeable draft; full Windows Job Object and Linux CMake/CTest still required.
- PR #5 — `agent/type-service-control-actions` → base PR #4. Fixed typed action vocabulary. Mergeable draft; result semantics remain non-substitutable.

Open critical Installer issues:

- #6 — platform-neutral service-control result. Windows raw SCM state and Linux `systemctl` exit status still escape behind one `int` port.
- #7 — exact stable-byte TUF/catalog/state admission. Current trust chain can authenticate one file and parse another after path replacement. Blocks signed-release trust and `PKG-E2E-011`.
- #8 — bounded acquisition/cache/archive expansion. Download totals/stalls/partials and archive expansion/types/ratios are not yet fully bounded. Blocks `PKG-E2E-011`.
- #9 — truthful transactional apply/rollback/crash recovery. Current mutation lifecycle lacks one durable transaction authority and can misreport rollback/commit. Blocks install lifecycle soundness and `PKG-E2E-011`.
- #10 — exact identity-bound uninstall and purge. Current check-then-delete path behavior and recursive deletion can remove bytes/tree not matching the reviewed plan. Blocks uninstall/purge soundness and `PKG-E2E-011`.

Installer merge/test order for current drafts:

1. PR #2
2. PR #3 independently onto #2
3. PR #4 onto #2
4. PR #5 onto #4
5. reconcile issues #6–#10 on final ancestry
6. run complete package/lifecycle evidence

### 4.2 Engine — chess/search/evaluation/model/inference/runtime authority

Authoritative architecture stack:

1. PR #39 — `agent/system-sanity`
2. PR #44 — `agent/model-adapter-contract` → base #39
3. PR #47 — `agent/integrate-engine-audit-stacks` → base #44

Reviewed child:

- PR #52 — `agent/canonical-fen-admission-v2` → base #47. Clean reconstruction of Jules FEN work. The public out-of-line `Position::from_fen` remains one authority; macro aliasing and the unchecked compile-definition path were removed. Static review and standalone strict compilation passed; full Engine build/probes/CTest remain required.

Inference state:

- Issue #45 is the broad neutral inference-port extraction and concrete ONNX delegation task.
- The first Jules attempt produced PR #49, a `main`-based 47-file setup snapshot with no inference implementation. PR #49 was closed.
- Issue #57 is the deliberately smaller provider-free contract slice.
- Jules task `12435499143853197163` produced PR #60, but PR #60 targets `main` and exposes the stale broad repository snapshot. It is **untrusted and not accepted**. Any useful contract/probe files must be isolated onto exact PR #47 ancestry and reviewed line by line.
- A second broad issue #45 Jules task, `11856418714977393425`, was retriggered. This overlaps the completed smaller #57 attempt. Do not accept overlapping outputs as separate authorities. Prefer the smaller provider-free contract first, then concrete ONNX delegation as a separate reviewed slice.
- Issue #46 composition root is blocked until the neutral inference boundary and concrete inference implementation are reviewed. UCI must not remain the concrete provider graph owner.

Transposition table state:

- Issue #43 / PR #51 first Jules TT attempt was rejected. It did not compile, duplicated the TT implementation in the test, used unsafe reset/spinning behavior, omitted required lock-free and memory proofs, and made misleading test claims.
- Issue #54 is the corrected production TT task. Jules PR #55 contained only planning/setup and a stale 43-file snapshot; it was closed.
- No accepted production lock-free TT implementation exists. The 4,096 sharded OS mutexes remain a critical per-node multicore blocker until a stable-or-miss production design is implemented, compiled, stress-tested against production code, memory-accounted, and benchmarked.

Additional Engine performance findings:

- #56 — profile and optimize machine-code hot paths only after inference/TT ownership stabilizes.
- #58 — remove shared killer/history synchronization and telemetry false sharing from the node hot path.
- #59 — replace repeated ~4 KiB MCGS repetition-history copies, linear scans, and full-capacity shifts with exact incremental state.

Engine acceptance sequence:

1. review/reconstruct provider-free inference PR #60 onto PR #47 or replace it;
2. implement concrete ONNX inference behind the neutral port;
3. implement composition root (#46);
4. implement and benchmark production TT (#54/#43);
5. run Engine correctness/substitution/provider parity/full CTest;
6. run representative one-thread and multicore useful-search benchmarks;
7. then address measured #56/#58/#59 work without conflating ownership changes.

### 4.3 Manager — installed-suite control-plane authority

Authoritative base:

- PR #54 — `agent/release-assembly` — Manager as neutral installed control plane, not Installer.

Current reviewed children:

- PR #73 — `agent/bound-match-plan-after-release-assembly` → base #54. Independent corrected match-plan admission. Validates by reference before copying, proves cardinality before allocation, discards caller pairings, bounds aggregate options, rejects invalid UTF-8/control characters and nonfinite SPRT values, and checks opening-index overflow. GCC, Clang, ASan, UBSan, and focused semantics passed; native Windows/Linux full tests remain required.
- PR #67 — `agent/harden-sqlite-store-foundation` → base #54. **Blocked by adversarial review; do not merge.**

Confirmed PR #67 gaps:

- `required_uint64()` calls `sqlite3_column_int64()` without requiring `SQLITE_INTEGER`; SQLite text and real storage classes silently coerce (`abc`→0, `1.5`→1).
- exact schema authority is incomplete: current-table creation can partially normalize malformed/unversioned storage before full shape proof.
- migration authority is not a constrained exact singleton schema contract.
- only the immediate parent directory is checked; every ancestor/junction/reparse/mount substitution is not proven safe.
- database/WAL/SHM checks are path-based and not bound to the opened database identity.
- test claims exceed coverage: missing exact storage-class corruption, negative stored projections, symlinked ancestor/directory, malformed/partial schema/index/constraint, pragma refusal, post-open replacement, and deterministic retry after initialization failure.
- the persistence spec requires backup verification before migration; PR #67 does not establish that lifecycle.

Manager delegated work:

- Issue #69 LocalAuth reconciliation. Current Jules task `15207249830771102373` is active. Preserve PR #54 loopback-origin proof and session/CSRF behavior; integrate exact lowercase key admission, non-symlink state, non-overwriting publication, permissions, rotation, and fail-closed pre-init behavior. Review every line.
- Issue #70 broad UCI-probe reconciliation. Current Jules task `5783702717709122040` is active. It overlaps the smaller pure-contract issue #76.
- Issue #76 defines the preferred small pure UCI identity/option/override contract. Two Jules task-creation attempts failed. Architecturally, implement/review #76 before integrating subprocess/path/discovery work from #70. Do not let a broad #70 PR bury a copied or weak parser.
- Issue #71 match-plan replay is superseded by independent PR #73. Jules PR #72 changed zero files while claiming four files and passing checks; it is closed and untrusted.

Manager next order:

1. finish/review #69 LocalAuth;
2. establish pure UCI contract #76, then reconcile #70 around it;
3. correct/reconstruct SQLite PR #67 with exact path, storage-class, schema, migration, pragma, retry, and test authority;
4. retain PR #73 on #54;
5. run spec check plus full Windows/Linux build/CTest and installed Manager authentication/probe/tournament traces.

### 4.4 Evidence Service — evidence scheduling/publication authority

Authoritative stack:

1. PR #10 — `agent/system-sanity`
2. PR #13 — strict CLI grammar
3. PR #14 — streaming dataset export
4. PR #15 — streaming dataset show
5. PR #16 — transactional crash-recoverable prune
6. PR #17 — integrate orphaned external-owner and target-identity fixes
7. PR #18 — exact config/pointer/owner-root admission
8. PR #19 — immutable daemon timing contract
9. PR #22 — resource configuration type/cardinality bounds

Important reviewed details:

- PR #18 is one coherent commit on PR #17. It adds bounded exact configuration reads, safe JSON merge/prototype-key rejection, exact owner directories, exact active pointer, descriptor-bound stable engine hashing, path/type bounds, and adversarial tests. Local Node 22 identity suite passed 13/13; configuration probes passed with a clearly documented local version-gate bypass. Node 24.15/full suite remains required.
- PR #19 freezes heartbeat/cycle/stale values at 15,000/60,000/300,000 ms while preserving config compatibility. It intentionally does not claim stale-status projection.
- PR #22 requires safe integers for idle samples/timers, retains continuous GiB/percentage thresholds, and bounds/uniquifies priority-process basenames.

Open Evidence issues:

- #20 — status records heartbeat timestamp but never projects missing/fresh/stale at the declared 300-second boundary.
- #21 — foreground revocation is not truly end-to-end two seconds: up to `pollMs` detection plus up to `revokeWithinMs` shutdown can exceed the contract.

All Evidence PRs #18/#19/#22 still require Node >=24.15.0, `npm run check`, `npm run acceptance:idle`, packaged runtime tests, and Windows/Linux behavior. Their Node 22 probes are not release evidence.

### 4.5 Book Forge — opening-book production authority

Current documented stack:

1. PR #8 — `agent/system-sanity`
2. PR #11 — configuration contract → base #8
3. PR #10 — daemon-lock cleanup → base #11

PR #9 is closed/superseded; its focused acquisition resource-probe test was carried into #11.

Adversarial override: broad PR #8, PR #11, and PR #10 are **blocked as acceptance evidence** until reopened findings are repaired.

Open critical issues:

- #12 — exact acquisition credential bytes at use time. Current credential `stat`/`readFile` follows/reopens paths and can send swapped token bytes.
- #13 — snapshot compiler must become single-pass enough and publication/recovery truthful. Current work is memory bounded but retains avoidable full passes/copies, predictable work paths, race/orphan and check-then-open publication defects.
- #14 — catalog and 64 shard SQLite foundation: exact paths/ancestors/WAL/SHM, no-follow identity, pragma verification, schema/migration/storage-class proof, typed translation, and recoverable cross-shard authority remain incomplete.

Reopened ledger rows:

- `BOOK-TRUST-017` — reopened.
- `BOOK-AUTH-018` — partially reopened; daemon-lock cleanup is not a complete transactional lifecycle and cleanup errors can replace primary failures.
- `BOOK-SCALE-020` — reopened.

Do not use PR #8's 59/59 aggregate count or deterministic candidate as proof that these reopened boundaries are sound.

### 4.6 Lichess Bot — Lichess provider authority

Current broad audit:

- PR #3 — `agent/system-sanity` — mergeable draft but **blocked by adversarial review**. Its 148-pass aggregate is not acceptance evidence for reopened boundaries.

Open critical issues:

- #4 — exact credential/profile/engine/runtime file admission and atomic publication. Token and identity files still have check-then-read/hash and symlink/replacement risks.
- #5 — byte-oriented UCI framing, typed identity/options, exact final applied-option identity, and hot-path allocation/copy removal. Automatic `Move Overhead`/`Ponder` are omitted from current applied-option hash; duplicate/conflicting option/identity grammar is insufficiently admitted.
- #6 — transactional game activation and nonblocking serialized status publication. Slot authority can be published before durable/session/task publication; synchronous status work can block game clocks.
- #7 — exact SQLite storage foundation, schema/migration/storage-class/pragma/path authority and deterministic retry.

Reopened ledger rows:

- `LICHESS-UCI-012`
- `LICHESS-CRED-013`
- `LICHESS-LIFECYCLE-014`
- persistence foundation separately reopened under #7

Do not merge PR #3 or promote a provider candidate until #4–#7 are resolved, line-reviewed, and exercised with loopback API, real process, storage, race, and active-game latency evidence.

## 5. Jules trust record and current tasks

### Rejected or superseded Jules outputs

- Engine PR #50 — main-based/stale FEN snapshot. Useful slice was reconstructed as clean PR #52; #50 closed.
- Engine PR #51 — TT code did not compile and the test duplicated the implementation; closed.
- Engine PR #55 — TT planning/setup only, no implementation, stale snapshot; closed.
- Engine PR #49 — inference environment/setup only, no port implementation, 47-file stale snapshot; closed.
- Manager PR #72 — empty commit/zero changed files with false replay/test narrative; closed.
- Manager match-plan work was independently reconstructed as PR #73.
- Engine PR #60 — completed smaller inference contract task, but targets `main` and carries stale snapshot changes. Untrusted; not accepted.

### Current task state at this snapshot

- Engine issue #45 task `11856418714977393425` — active broad inference task.
- Manager issue #69 task `15207249830771102373` — active LocalAuth task.
- Manager issue #70 task `5783702717709122040` — active broad UCI-probe task.
- Engine issue #57 task `12435499143853197163` — completed as untrusted PR #60.
- Manager issue #76 smaller UCI contract — task creation failed twice and is not active.

### Mandatory Jules review protocol

For every Jules result:

1. Confirm task is actually complete; intermediate error comments are not task completion.
2. Verify requested base branch and compare against that base.
3. List changed filenames before reading implementation.
4. Reject full-repository snapshots and unrelated ownership changes.
5. Read every changed line and every omitted requirement.
6. Verify declarations, aggregation, call sites, error paths, lifetimes, races, and cleanup.
7. Ensure tests call production code rather than a copied implementation.
8. Reproduce compile/test claims or mark them unverified.
9. Check test fixtures reach the intended assertion rather than failing earlier.
10. Check memory, allocation, copy, blocking, and hot-path consequences.
11. Reconstruct accepted files onto authoritative ancestry rather than merging the Jules branch wholesale.
12. Keep PR draft until all required native/platform/package gates pass.

## 6. Repairs produced during the current continuation

### Accepted as reviewable drafts, not release-ready

- Engine PR #52 — canonical FEN admission on correct ancestry.
- Manager PR #73 — bounded match planning before allocation.
- Evidence PR #18 — exact path/config/pointer/engine identity admission.
- Evidence PR #19 — canonical immutable daemon timing.
- Evidence PR #22 — resource config types/counts/process-name bounds.
- Installer PRs #3/#4/#5 — focused asset, process-tree, and typed action children, still requiring platform-native gates.

### New explicit blocker issues

- Evidence #20 — stale heartbeat projection.
- Evidence #21 — end-to-end foreground revocation budget.
- Installer #6–#10 — substitutable service result, stable-byte trust, bounded acquisition/archive, transactional mutation/recovery, exact uninstall/purge.
- Engine #56/#58/#59 — measured hot-path, heuristic synchronization, repetition history.
- Lichess #4–#7.
- Book Forge #12–#14.
- Manager #76 smaller UCI contract.

## 7. Immediate restart procedure for the next agent/chat

Do this in order:

1. Read this handoff and the four canonical audit/architecture files listed at the top.
2. Fetch current heads and compare every listed branch; do not assume the SHAs in old PR bodies remain current.
3. Check Jules task comments for #45, #69, and #70. Do not create more tasks while three are active.
4. Review Engine PR #60 as hostile input. Because it targets `main`, isolate only the issue #57-owned files onto a review branch based on PR #47. Reject unrelated release/governance/evaluation changes. Compile the provider-free target before accepting anything.
5. When #45, #69, or #70 completes, apply the mandatory Jules review protocol. Broad #70 must not replace the smaller #76 pure-parser ownership design.
6. Continue Manager SQLite PR #67 review. Correct storage-class proof first; then exact ancestor/path/open identity, schema/migration/pragma authority, failure reset, and adversarial tests. Do not merge the current PR.
7. Keep Manager PR #73 draft until full native suites pass.
8. Continue Lichess issues #4–#7 in owner-sized branches; PR #3 remains blocked.
9. Continue Book Forge issues #12–#14; PRs #8/#11/#10 remain blocked as acceptance evidence.
10. Implement Evidence #20/#21 only on final #22 ancestry and run Node 24.15/full/provider acceptance.
11. Address Installer issues #7–#10 before any package-cycle conclusion. These are critical trust and destructive-lifecycle blockers.
12. Update the ledger statuses in one convergence commit after the corresponding fixes enter authoritative ancestry. Until then the PR #2 adversarial override supersedes older `corrected` rows.
13. Before stopping work, update this handoff with new PR/issue/task IDs, rejected approaches, exact validation, remaining blockers, and the next ownership boundary.

## 8. Final required gates before a soundness decision

### Source and architecture

- all active specs/preflight/boundary checks pass;
- no orphan fixes outside candidate ancestry;
- no copied ownership or source-tree imports;
- all neutral substitution probes pass;
- all reopened ledger rows are reclassified with concrete evidence.

### Native and runtime

- Windows and Linux native configure/build/CTest for Engine, Manager, and Installer;
- Node >=24.15 full suites for Evidence and Book Forge;
- complete Lichess loopback API and active-game lifecycle tests;
- real ONNX/provider parity, cancellation, reload, and composition traces;
- TT correctness, race, memory, one-thread and multicore useful-search benchmarks;
- Evidence/Book resource preemption and idle acceptance;
- storage reopen/migration/corruption/race/recovery tests on both platforms.

### Package and lifecycle

- clean immutable provider candidates from accepted ancestry;
- exact manifests, receipts, inventories, digests, SBOMs, provenance, and dependency closure;
- Windows personal and all-users package behavior;
- Linux package behavior;
- clean install;
- ordinary use across Engine, tournaments, Lichess, Book Forge, Evidence;
- repair;
- upgrade;
- reinstall;
- restart/crash recovery at injected checkpoints;
- normal uninstall with preservation proof;
- reinstall with safe revalidation and no retained-state authority confusion;
- explicit previewed purge with exact ownership and symlink/reparse safety;
- repeated cycle with no stale startup, receipt, lock, cache, or transaction authority.

No partial source-level success substitutes for these gates.

## 9. Handoff maintenance rule

This file must be updated whenever any of the following changes:

- authoritative branch ancestry;
- a finding changes status;
- a Jules task starts, completes, fails, or produces a PR;
- a PR is accepted, blocked, superseded, reconstructed, closed, or merged;
- a new critical issue is opened;
- validation is executed;
- the next restart action changes.

A handoff is incomplete if a new agent would need conversation memory to determine what is authoritative, what was rejected, what was tested, or what to do next.
