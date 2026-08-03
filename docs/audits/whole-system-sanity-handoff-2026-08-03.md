# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release conclusion exists**  
Snapshot time: 2026-08-03 16:24 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`

This is the canonical continuation handoff for the six-repository sanity audit. It supersedes the earlier 15:00 and 16:10 snapshots while preserving their commits and issue/PR comments as immutable evidence. A later agent must be able to determine authoritative ancestry, rejected work, active temporary repositories, validation status, and the next ownership boundary without conversation history.

## 1. Governing authority and method

Read, in order:

1. `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`
2. `docs/audits/whole-system-sanity-2026-08.md`
3. `docs/audits/whole-system-performance-addendum-2026-08-03.md`
4. `docs/audits/whole-system-branch-topology-addendum-2026-08-03.md`
5. this handoff and its YAML companion
6. each touched repository's `AGENTS.md`, read-first specification, active spec registry, issue body, and hostile-review comments named below

Non-negotiable rules:

- project-owned neutral LEGO contracts at repository and component boundaries;
- one authority per concern; Manager is not Installer and providers own private domains;
- branch ancestry is product correctness;
- immutable artifacts/manifests/receipts replace peer-source imports and hidden discovery;
- no JSON, filesystem, blocking locks, repeated transformations, avoidable allocation/copy, or per-element dispatch in hot loops;
- domain-appropriate exact types, ranges, identities, schemas, and lifecycle authority;
- exact evidence before claims; aggregate test counts and PR prose are not proof;
- Jules output is hostile input until its authored commit, omissions, call sites, tests, races, cleanup, and performance are reviewed;
- repairs are ownership-sized, not arbitrary patches or broad cross-owner rewrites;
- source review cannot establish release readiness without native, packaged, destructive, recovery, and performance evidence.

Automatic CI remains disabled by project policy. Required validation is local/manual and must be recorded exactly.

## 2. Environment and validation limits

The connected GitHub tool can inspect and modify the private repositories. It does not provide the complete private checkouts, Node 24.15 runtime, Windows/Linux native toolchains, installed provider payloads, real ONNX-linked Engine environment, or complete installer lifecycle environment.

No final evidence currently exists for:

- complete Windows/Linux Engine, Manager, or Installer configure/build/CTest on final ancestry;
- Node >=24.15 complete Evidence, Book Forge, or Lichess installed-package suites;
- current real ONNX parity, cancellation, reload, and composition;
- current representative one-thread/multicore useful-search benchmarks;
- complete clean install → use → repair → upgrade → reinstall → recovery → uninstall → purge.

Connector review and focused source evidence remain useful but are not native or release evidence.

## 3. Current conclusion

The sanity check is **not complete**. Critical trust, exact-file, storage, lifecycle, inference, search-concurrency, provider, Installer transaction, uninstall, and packaged end-to-end findings remain open.

`PKG-E2E-011` remains unknown. No soundness conclusion and no release-readiness conclusion may be issued.

## 4. Repository state

### 4.1 Installer — suite composition and deployment authority

Authoritative base: PR #2, `agent/release-assembly`.

Reviewed children:

- PR #3 bounded indexed-asset results → #2; native Windows/Linux integration still required.
- PR #4 native process-tree stop → #2; Windows Job Object and Linux native evidence still required.
- PR #5 typed service-control actions → #4; result semantics remain non-substitutable.

Open critical issues:

- #6 platform-neutral service-control result;
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
- PR #61 review is recorded in comment `5172691145`; its task-owned slice used forbidden `void*`, hot-path vectors, incomplete identity/cancellation/error contracts, unsafe concurrent provider objects, and relocated model/chess semantics without required parity/performance proof.

Transposition-table issues #43/#54 remain open; PRs #51/#55 remain rejected. Performance issues #56/#58/#59 remain open.

New shared-runtime prerequisite:

- issue #62, **Expose timestamped typed foreground revocation from the shared resource lease**;
- prepared branch `agent/timestamp-resource-lease-revocation`, based exactly on PR #47 ancestry;
- owns a provider-neutral typed revocation observation/notification carrying the earliest foreground request visibility time for Evidence #21;
- no Jules label or task was launched.

Next Engine order:

1. create a private disposable workbench from exact PR #47 for issue #57, or implement directly on #47 ancestry;
2. accept the small fixed/bounded typed provider-free contract before broad ONNX work;
3. implement a small ONNX adapter preserving exact model/adapter/tuning identity and safe concurrency;
4. implement composition root #46;
5. implement and benchmark production TT;
6. review/land shared-runtime issue #62 independently of inference work.

### 4.3 Manager — installed-suite control-plane authority

Authoritative base: PR #54, `agent/release-assembly`.

Reviewable child: PR #73 bounded match-plan admission → #54; complete native suites remain.

Blocked storage child: PR #67 — **do not merge**. Review comments record coercive storage projection, unsafe ancestor creation, unbound database/WAL/SHM identity, schema mutation before classification, missing verified backup, incomplete migration/integrity authority, and unproven deterministic failure recovery. Latest detailed review ID: `4848995624`.

LocalAuth issue #69 remains open. Jules PR #77 was rejected and closed; comment `5172704927` records unsafe path/key admission, nonexclusive/nondurable publication, no Windows ACL authority, ambiguous committed cleanup failure, and cached bearer authority surviving malformed/unsafe rotation.

UCI issues #70/#76 remain open. Issue #76 is the preferred pure identity/option/override contract before broad process/path integration. Jules PR #74 was rejected and closed.

Next Manager order:

1. reconstruct #69 on exact PR #54 ancestry, preferably through a private workbench;
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

Issue #20 heartbeat status projection:

- prepared branch `agent/project-daemon-heartbeat-staleness`, based on final current PR #22 ancestry;
- canonical expanded review comment `5172783286`;
- one pure liveness projector must parse exact canonical timestamps with an injected clock and expose `missing|fresh|stale`, nonnegative bounded age, and fixed 300000 ms threshold without implying process/lock ownership;
- public/Manager status must consume the typed projection rather than recalculate it;
- no Jules task launched.

Issue #21 foreground revocation deadline:

- prepared branch `agent/end-to-end-foreground-revocation`, based on PR #22 ancestry;
- canonical expanded review comment `5172787455`;
- blocked on Engine shared-runtime issue #62;
- current loop can spend almost one second waiting for the next poll, then await resource probing, then start a fresh two-second shutdown allowance, and releases the cooperative lease only after later cleanup;
- required design uses the earliest request visibility timestamp, one absolute 2000 ms deadline, remaining-budget tree shutdown, verified descendant termination, and immediate lease release before guard/catalog/status cleanup;
- no Jules task launched.

All Evidence work still requires Node >=24.15, `npm run check`, `npm run acceptance:idle`, package-installed Windows/Linux behavior, and destructive recovery evidence.

### 4.5 Book Forge — opening-book production authority

PRs #8/#11/#10 remain blocked as acceptance evidence.

Issue #12 exact acquisition credential bytes:

- canonical expanded comment `5172735375`;
- current `stat` then `readFile` can read different objects, decodes/materializes before enforcing the byte limit, and `trim()` silently changes credential authority;
- existing tests incorrectly place credentials inside the workspace after validation and accept terminal newlines; those fixtures must be reversed;
- required owner performs exact descriptor/handle admission at use time, physical ownership proof, 4 KiB plus one-byte overflow detection, exact byte grammar, callback-scoped use, no cache, and generic redacted external diagnostics.

Issue #13 recoverable snapshot compilation/publication:

- canonical expanded comment `5172750056`;
- current lifecycle uses predictable recursively removed work paths, no build lease, unawaited writer destruction, repeated selected-row scans/full-artifact rereads, path-based validation, filesystem activation before catalog/frontier authority, and unsafe publisher rollback;
- required one `SnapshotBuildOperation` owns lease, private workspace, one selected-row fanout pass, one independent stable-descriptor validation pass, versioned receipt, filesystem/catalog/channel/frontier reconciliation, and bounded cleanup evidence.

Issue #14 catalog and 64-shard SQLite authority:

- canonical expanded comment `5172758661`;
- current storage creates current tables before version classification, uses path-only catalog/shard/WAL/SHM access, coercive `Number`/raw-row projection, incomplete migrations, and split cross-shard transactions;
- existing tests normalize partial versioned schemas rather than reject them;
- required local native SQLite/VFS or isolated storage owner, exact schema/application/version identity, BigInt/exact storage-class translation, verified backup/migration/recovery, and catalog-backed cross-shard operation receipts.

Reopened ledger rows remain `BOOK-TRUST-017`, `BOOK-AUTH-018`, and `BOOK-SCALE-020`.

### 4.6 Lichess Bot — Lichess provider authority

PR #3 remains blocked; its aggregate passing count is not acceptance evidence.

Canonical issue comments:

- #4 exact credential/profile/engine/runtime file owner — `5172632661`;
- #5 byte UCI framing, typed options, exact final applied identity — `5172640649`;
- #6 transactional game activation and off-loop serialized status — `5172653544`;
- #7 exact SQLite foundation — `5172666890`.

Required ownership sequence:

1. exact-file/platform identity and durable publication;
2. raw-byte UCI framing plus one immutable qualification identity shared by capacity/smoke/timing/provenance;
3. durable game activation nonce/phase, exact slot correlation, structured terminal cleanup, latest-wins off-loop status publisher;
4. exact database/WAL/SHM owner, typed translation, schema/migration/recovery.

Do not merge PR #3 or promote a provider candidate until #4–#7 pass real-process, loopback API, storage, startup, active-game latency, package, race, and recovery evidence.

## 5. Jules trust and disposable-workbench protocol

Observed rule: issue-label Jules tasks repeatedly started from repository default `main`, even when issue text named a prepared branch. Intermediate Jules error comments are not terminal; completed PRs remain untrusted until the authored commit is reviewed.

Rejected/closed outputs include Engine #49/#50/#51/#55/#60/#61 and Manager #72/#74/#77.

A workbench is a temporary transaction, not a development repository:

`created → baseline_verified → jules_active → result_frozen → transferred → equivalence_verified → destination_pr_open → deleted`

Required workflow:

1. create one private `iteathen/jules-wb-<project>-<issue>-<attempt>` repository and authorize Jules;
2. flatten only exact accepted prerequisite ancestry into workbench `main`;
3. commit a baseline manifest with source repo/branch/SHA, destination branch, task, permitted owners/files, and cleanup state;
4. run one task only and never reuse the repository;
5. freeze the result SHA and extract only `baseline..result`;
6. transfer the accepted delta to a fresh branch from the original authoritative ancestry;
7. verify patch equivalence and perform hostile review in the original repository;
8. record all SHAs/PR/transcript, remove Jules access if separately granted, delete the workbench repository and local clones/worktrees, and mark state `deleted` here.

Current registry:

- no `jules-wb-*` repository is visible through the connected GitHub installation;
- no active workbench exists;
- the connected GitHub capability can create branches, files, issues, commits, and PRs inside existing repositories but **cannot create the new private repository** required for a safe workbench;
- do not repoint or contaminate any product repository's real `main` as a substitute;
- creation of the first private workbench is therefore an explicit user/account-side gate.

## 6. Immediate restart order

1. Read this handoff, YAML, architecture, ledger, performance, and topology documents.
2. Fetch current heads before trusting any recorded SHA.
3. Confirm no abandoned `jules-wb-*` repository exists before creating one.
4. User creates/authorizes the first private Engine issue #57 workbench; seed its `main` from exact PR #47 and register it here before applying a Jules label.
5. Review/implement Engine shared-runtime issue #62 independently.
6. Implement Evidence #20 on its prepared PR #22-based branch.
7. After #62 is accepted and vendored exactly, implement Evidence #21 on its prepared branch.
8. Reconstruct Manager #69, #76/#70, and PR #67 on exact #54 ancestry.
9. Continue Lichess #4–#7 in ownership order.
10. Continue Book Forge #12–#14 in ownership order.
11. Resolve Installer #7–#10 before any package/lifecycle conclusion.
12. Converge ledger statuses only after accepted fixes enter authoritative ancestry.
13. Before stopping, update both handoff files, the workbench registry, exact validation, and the discoverability comment on Installer PR #2.

## 7. Final gates before any soundness decision

Source/architecture:

- all active specs, preflight, and boundary checks pass;
- one authoritative ancestry per repository with no orphan fixes;
- no copied ownership or peer-source imports;
- all substitution probes pass;
- all reopened ledger rows are reclassified with concrete evidence.

Native/runtime:

- Windows/Linux Engine, Manager, and Installer configure/build/CTest;
- Node >=24.15 complete Evidence and Book Forge suites;
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

Update this file and its YAML companion whenever authoritative ancestry, a finding, Jules task/result, workbench state, PR/issue disposition, validation result, or the next restart action changes.

The handoff is incomplete if a new agent would need conversation memory to determine what is authoritative, rejected, tested, temporary, or next.
