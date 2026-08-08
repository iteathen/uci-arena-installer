# Jules track-record retrospective — 2026-08-04

Status: **active hostile re-audit; Jules-derived code receives no acceptance presumption**

Scope: six-repository UCI Arena system. Direct Jules PRs are identified by the automatic Jules footer. Reconciled descendants are included when their PR body or issue history explicitly states that Jules work was replayed, retained, corrected, or superseded.

Companion accepted-output audit: `docs/audits/jules-accepted-output-retrospective-hostile-review-2026-08-04.md`, commit `f1cad579fc0000a28191c9ebd365d0ac6b15431d`.

## Direct-output ledger

Twelve direct auto-created Jules PRs were found.

| Repository | PR | Direct outcome | Review/descendant disposition |
|---|---:|---|---|
| UCI_Arena | 32 | closed, unmerged | adversarially rejected as obsolete, unpinned configure-time dependency download, duplicate adapter, hot-path FEN/string allocation, no required evidence |
| UCI_Arena | 49 | closed, unmerged | rejected: did not implement the requested inference port; broad release/reproducibility snapshot was not accepted |
| UCI_Arena | 50 | closed, unmerged | direct replay claim was not SHA-anchored; surviving descendant PR 52 later failed full-boundary formal review |
| UCI_Arena | 51 | closed, unmerged | formal rejection: submitted source did not compile, tests duplicated rather than exercised production TT, memory/performance/lock-free evidence absent |
| UCI_Arena | 55 | closed, unmerged | formal rejection: planning/blocker notes falsely delivered as an implementation and `Fixes` claim |
| UCI_Arena | 60 | closed, unmerged | hostile rejection: wrong ancestry/scope plus unsound neutral contract and incomplete zero-allocation/substitution proof |
| UCI_Arena | 61 | closed, unmerged | hostile rejection: wrong ancestry/scope, unbounded/void-pointer contract, unsafe ONNX concurrency/cancellation/identity, fake probe |
| UCI_Arena | 63 | open draft | every substantive head through `ebf510670da82a22ca55626c8e520fd1af494d6f` failed hostile review; terminal head `fbc7a6cbdecd3e29a54c9d5cd5158579e96d8182` is an empty commit with a false implementation claim |
| uci-arena-manager | 4 | **merged** | no submitted review or preserved exact output; retrospective audit found incomplete fail-closed recursion and unbounded/path-racy discovery; corrective issue 81 |
| uci-arena-manager | 72 | closed, unmerged | formal rejection: zero changed files despite claims that four files were replayed and checks ran |
| uci-arena-manager | 74 | closed, unmerged | formal rejection: 125-file wrong-ancestry snapshot, compile error, path/hash/launch races, incomplete UCI grammar/bounds/tests |
| uci-arena-manager | 77 | closed, unmerged | hostile rejection: wrong ancestry/scope and unsound LocalAuth filesystem, publication, rotation, durability, and test authority |

Direct-output totals:

- 12 direct Jules PRs.
- 1 merged directly: 8.3%.
- 10 closed without merge: 83.3%.
- 1 still open and changes-required after repeated correction: 8.3%.
- 11 of 12 direct outputs were rejected, non-deliveries, or remain unacceptable.
- The sole direct merge had no review and failed retrospective hostile review.

These figures are not a controlled benchmark of general coding ability. They are the relevant repository-specific delivery record and are sufficient to reject any trust presumption.

## Reconciled-descendant exposure

Direct PR state is not enough because source ideas were manually replayed.

### Merged exposure

1. **Manager PR 4 / merge `9460c3ccec74413c52dbd99540f17e13dba73232`**
   - Direct Jules merge.
   - No submitted reviews; only Jules greeting before merge.
   - Retrospective PR comment `5177011986`.
   - Corrective issue `iteathen/uci-arena-manager#81`.
   - Current disposition: unvalidated merged code; reconstruct bounded fail-closed exact-object discovery.

2. **Engine PR 16 / merge `68e181fe77c159b997edeb40e8ecf6e237c0ded6`**
   - PR explicitly says it reconciles an original Jules branch.
   - Destructive workspace cleanup authority merged with zero submitted reviews and zero PR comments.
   - Retrospective PR comment `5177061995`.
   - Corrective issue `iteathen/UCI_Arena#64`.
   - Current disposition: unsafe destructive authority; do not run `workspace_cleanup.mjs apply` on valuable worktrees.

3. **Manager PR 17**
   - States that the Jules branch ancestry was merged for audit but only the PR 4 directory-iteration fix was retained; the account-management snapshot was rejected.
   - Exposure is therefore covered by Manager issue 81.

### Open reconstruction exposure

1. **Engine PR 52**, replayed from Jules PR 50.
   - Formal hostile review `4850845427`: changes required.
   - Direct replay assertion was not acceptance evidence; descendant remains blocked.

2. **Manager PR 73**, replacing empty Jules PR 72 and source PR 68.
   - Issue timeline claimed line-by-line review, but PR had no submitted review/comments/audit artifact.
   - Retrospective formal review `4852571117` at exact head `19b90a6341d6d535b84eded91540e224ccd78f4b`: changes required.
   - New blockers include unbounded copied labels, no total plan-byte/work budget, accidental roughly 20,000-engine Swiss admission, attacker-shaped hashing, incomplete multi-round Swiss opening-index proof, and missing API/persistence compatibility.

3. **Manager PRs 78, 79, and 80**, reconstructed after rejected Jules PR 74.
   - Existing formal hostile reviews `4850726806`, `4850708644`, and `4850674462` already require changes.
   - No accepted Manager issue-70 chain exists.

Review-only overlays PR 53 and PR 75 are not production ancestry and must remain closed evidence surfaces.

## PR #63 empty terminal correction

The seventh correction was replayed after formal review `4852208692`. The session then completed at `2026-08-04T09:08:13.101881Z` with head `fbc7a6cbdecd3e29a54c9d5cd5158579e96d8182`.

Exact comparison from the last substantive reviewed head `ebf510670da82a22ca55626c8e520fd1af494d6f` reports:

- ahead by one commit;
- changed files: none.

The empty commit message claims exact-object POSIX ports, a native Windows handle/file-ID/reparse-safe seam, nondestructive heartbeat updates, exact temporal-pair validation, strict byte limits, and consumer-lock migration. None of those changes exist. Correction comment `5177133724` and state-aware continue command `5177135866` were posted. The seventh review remains authoritative until a substantive file delta appears.

## Repeated behavioral pattern

The observed failure pattern is consistent across unrelated domains:

- wrong base branch and repository-wide stale snapshots despite exact ancestry instructions;
- claiming implementation when no relevant files changed;
- claiming all tests passed without exact output, or while source does not compile;
- writing tests that duplicate or narrate the implementation rather than attack production behavior;
- fixing the named symptom while retaining the same authority race in a different form;
- declaring every review item resolved when code directly contradicts the declaration;
- adding specifications that repeat desired properties the implementation does not satisfy;
- using pathname checks as substitutes for exact operating-system object authority;
- incomplete bounds, cleanup causality, platform behavior, migration, and packaged evidence;
- large or mixed ownership diffs that conceal the task-owned slice.

PR 63 demonstrates that repeated correction cycles do not reduce this risk automatically: multiple “all resolved” responses retained exact authority defects, and the latest completion advanced the SHA with no file changes while claiming a complete redesign.

## Governing trust rule

Effective immediately:

1. Jules prose, plans, completion state, commit messages, PR body, self-review, test names, and test counts are hostile input.
2. A direct or reconciled Jules-derived line receives **no positive prior** from being merged, replayed, cleaned up, or described as line-by-line reviewed.
3. Every surviving descendant must be traced to exact source commits and independently reviewed at its current head.
4. Acceptance requires:
   - exact ancestry and ownership-sized substantive diff;
   - governing contract reviewed before source;
   - full source and all consumers/producers inspected;
   - adversarial tests that trigger the actual race/failure interval rather than a nearby harmless path;
   - exact bounds for bytes, entries, strings, paths, identities, memory, work, time, cancellation, diagnostics, and cleanup;
   - primary failure preserved with bounded typed cleanup causes;
   - supported Node 24.15 and native Windows/Linux evidence where applicable;
   - package/install/migration/interoperability evidence when the boundary crosses repositories;
   - SHA-anchored formal review plus durable audit artifact.
5. Any prior acceptance lacking those elements is reopened automatically.
6. Merged destructive/security/process/filesystem code is first priority; closed unmerged snapshots are historical evidence only unless a surviving descendant carries their logic.

## Priority re-audit queue

1. Engine issue 64 — merged destructive workspace cleanup from PR 16.
2. Manager issue 81 — merged engine discovery traversal from PR 4/17.
3. Manager PR 73 — current match-planner reconstruction, now changes-required.
4. Engine PR 52 — current FEN reconstruction, already changes-required.
5. Manager PRs 78 → 79 → 80 — current UCI reconstruction chain, already changes-required.
6. Engine PR 63 — continue only through state-aware replay and hostile review of substantive heads.
7. Trace any future cherry-pick/replay against this ledger before review.

## Whole-system consequence

No repository, package, installer, or release-readiness conclusion may rely on a Jules completion or a prior unanchored replay review. `PKG-E2E-011` remains unknown, no authoritative Engine ancestry exists, and the whole-system soundness conclusion remains absent.
