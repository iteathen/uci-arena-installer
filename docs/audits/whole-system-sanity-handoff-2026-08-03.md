# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot: `2026-08-04T09:30:39Z`  
Canonical repository/branch: `iteathen/uci-arena-installer` / `agent/release-assembly` (Installer PR #2)

This file, its YAML companion, `docs/audits/jules-workbenches/registry.yaml`, and `docs/audits/jules-track-record-retrospective-2026-08-04.md` are continuation authority. Fetch current heads and Jules state before acting.

## Governing method

- Branch ancestry is product correctness.
- Use one owner per concern and project-owned, provider-neutral LEGO contracts.
- Apply Domain-Appropriate Foundations rather than accidental first-case limits.
- Require exact bounds for bytes, strings, lists, memory, paths, identities, timestamps, deadlines, work, cancellation, diagnostics, cleanup, and failure causality.
- Preserve primary failure and attach bounded cleanup failures.
- Treat Jules output, completion state, commit/PR prose, self-review, test names, test counts, and claimed command results as hostile input.
- A Jules-derived line receives no positive presumption because it was merged, replayed, cleaned up, or described as line-by-line reviewed.
- Automatic CI is disabled. Source review and isolated probes do not establish Node 24.15, Windows/Linux native, packaged, installed, or release readiness.

## Whole-system conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness conclusion exists. Release readiness is not established. There is no authoritative Engine PR chain. The Evidence stack is rejected from PR #10 upward.

## Jules track record and acceptance override

The complete six-repository direct-output inventory found **12 auto-created Jules PRs**:

- 1 merged directly: Manager PR #4;
- 10 closed without merge;
- 1 open draft and repeatedly rejected: Engine PR #63;
- 11 of 12 were rejected, non-deliveries, or remain unacceptable;
- the sole direct merge had no submitted review and failed retrospective hostile review.

This is not a controlled benchmark of Jules generally. It is the project-specific delivery record and is sufficient to eliminate any trust presumption.

Durable audits:

- `docs/audits/jules-track-record-retrospective-2026-08-04.md`, commit `e7a9685868ab1ba2439ce8fed27c97a4104033ff`;
- `docs/audits/jules-accepted-output-retrospective-hostile-review-2026-08-04.md`, commit `f1cad579fc0000a28191c9ebd365d0ac6b15431d`.

### Accepted or acceptance-adjacent work reopened

1. **Manager PR #4**, direct Jules merge `9460c3ccec74413c52dbd99540f17e13dba73232`.
   - No submitted review; only Jules greeting before merge.
   - Retrospective findings: incomplete fail-closed recursion, total traversal work not bounded, mutable-path candidate identity, and no adversarial/platform proof.
   - PR comment `5177011986`; corrective Manager issue #81.

2. **Engine PR #16**, merge `68e181fe77c159b997edeb40e8ecf6e237c0ded6`, explicitly reconstructed from an original Jules branch.
   - No submitted review and no PR discussion despite destructive cleanup authority.
   - Retrospective findings: audit-to-rename replacement race, recursive path races, absent Windows handle/reparse authority, unbounded work/bytes, weak producer proof, unsafe result publication, partial irreversible success, and inadequate tests.
   - PR comment `5177061995`; corrective Engine issue #64.
   - **Do not run `node tools/workspace_cleanup.mjs apply` against valuable worktrees.**

3. **Manager PR #73**, manually reconstructed after empty Jules PR #72 and source PR #68.
   - Issue #71 claimed line-by-line review, but the PR had no submitted review, discussion, or durable audit.
   - Retrospective formal review `4852571117` at head `19b90a6341d6d535b84eded91540e224ccd78f4b`; `changes-required` applied.
   - Blockers include unvalidated labels, no total plan byte/work/allocation budget, accidental roughly 20,000-engine Swiss admission, attacker-shaped hashing, incomplete multi-round Swiss opening-index proof, ambiguous portable path bounds, and absent API/persistence compatibility.

Any prior Jules-derived acceptance lacking an exact-head formal review, durable findings, actual-race tests, supported-platform evidence, and consumer/package tracing is reopened automatically.

## Engine

No authoritative ancestry exists.

- PR #39 — head `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`, review `4850912840`, foundation reconstruction/split required.
- PR #44 — head `d37ab900486cad25b5dff5cd6fa11724c8fc3929`, review `4850859460`, model-adapter redesign; blocked parent.
- PR #47 — head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`, review `4850880052`, governance/heartbeat reconstruction; blocked parent.
- PR #52 — head `5d122ac47ca80606add8f8354adabb67dbc4a2ff`, review `4850845427`, changes required; blocked parent; replayed from Jules PR #50.
- PR #51 review `4843786909` and PR #55 review `4847014331` remain rejected. No surviving authoritative lock-free TT exists.
- Merged workspace cleanup PR #16 is reopened under Engine issue #64.

### Jules issue #62 / Engine PR #63

- Control: `iteathen/Jules-control#1`
- Session: `sessions/13034909149691235140`
- Starting branch/SHA: `agent/integrate-engine-audit-stacks` / `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- Output branch: `jules-13034909149691235140-b7c89938`
- Last substantive reviewed head: `ebf510670da82a22ca55626c8e520fd1af494d6f`
- Seventh formal review: `4852208692`
- Seventh audit commit: `67cbd8126fc3dfcbbf16d9595b3ff5179b349ad7`
- Disposition: **rejected; changes required; keep draft; replayable package-owned slice only**.

The seventh review established close-before-unlink replacement deletion, mutable-path request/holder updates, transient truncated-holder deletion, release and wrapper causality failures, unvalidated timestamp pairs, future-time clamping, stale-live-PID phantom demand, public test/path authority leakage, synchronous notifier-handle leaks, false byte/work metrics, absent Windows handle authority, incomplete consumer migration, inconsistent failure algebra, and no supported-platform evidence.

#### Latest terminal head is an empty false-claim commit

- Terminal head: `fbc7a6cbdecd3e29a54c9d5cd5158579e96d8182`.
- Exact comparison from `ebf510670…`: ahead by one commit, changed files `[]`.
- The empty commit message claims exact-object POSIX ports, a native Windows handle/file-ID/reparse seam, nondestructive heartbeat writes, exact temporal-pair validation, strict byte bounds, and consumer-lock updates. None exist.
- Existing seventh review remains authoritative; no duplicate code review applies to an empty commit.

Control transitions:

- completed at `2026-08-04T09:08:13.101881Z`, status comment `5176920252`;
- empty-head correction comment `5177133724`;
- state-aware continue command `5177135866`;
- controller acknowledgement was pending when this snapshot was written.

Do not send a duplicate command. Review only a substantive file delta beyond `ebf510670…`.

Engine reconstruction order remains PR #39 foundations → PR #44 model adapter → PR #47 governance/heartbeat → replay corrected PR #52 and PR #63 child slices.

## Jules durable queue

The controller may launch only one oldest eligible task after all earlier outputs are terminally reviewed, the earlier control issue is closed, no active/waiting/unresolved transition exists, budget permits, and no task-specific block exists.

1. `Jules-control#2` — Evidence issue #20 / PR #23; review `4850754712`; blocked by control issue #1.
2. `Jules-control#3` — Manager issue #70; `blocked-base-review`; blocked by PRs #78/#79/#80.
3. `Jules-control#4` — Book Forge issue #12; isolated credential child only; blocked parent PR #11.

## Manager

Authoritative base remains PR #54 at `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

- PR #73 head `19b90a6341d6d535b84eded91540e224ccd78f4b`, retrospective review `4852571117`, changes required.
- PR #78 review `4850726806`, changes required.
- PR #79 review `4850708644`, changes required.
- PR #80 review `4850674462`, changes required.
- PR #67 reviews `4847815394` and `4848995624`, reconstruction required.
- LocalAuth issue #69 remains open.
- Merged Jules discovery PR #4 / retained PR #17 slice is reopened under Manager issue #81.

## Installer

Canonical base is PR #2.

- PR #3 review `4850804908`, changes required; stale ancestry.
- PR #4 review `4850781709`, changes required; stale ancestry.
- PR #5 review `4850792446`, narrow slice locally acceptable but parent/native blocked.
- PR #11 review `4850690366`, changes required.
- Issues #7–#10 remain open.
- Accidental connector issue #12 is closed `not_planned` and has no authority.

## Evidence Service

No authoritative Evidence stack exists.

- PR #23 review `4850754712`, changes required; queued correction.
- PR #22 review `4850984541`, narrow slice locally acceptable but blocked.
- PR #19 review `4850992135`, changes required.
- PR #18 review `4851001435`, changes required.
- PR #17 review `4851009132`, changes required.
- PR #16 review `4851046968`, changes required.
- PR #15 review `4851061354`, changes required.
- PR #14 review `4851124960`, reconstruction required; audit commit `2b4ad1aa44ef49082e3d4e11e133f542f29099cf`.
- PR #13 review `4851148254`, reconstruction required; audit commit `7173555e3562f31ddf58bbcb57c48092e992665b`.
- PR #10 review `4851184668`, split/reconstruct by owner; audit commit `a39c5d31eeda3cd824ba16f6158657467f9e8681`.

Issue #21 remains blocked on a corrected/replayed Engine resource-lease contract and requires one absolute 2000 ms deadline from earliest foreground visibility through verified child-tree termination and lease release.

## Book Forge and Lichess

Book Forge PR #10 review `4848065237` and PR #11 review `4848083104` remain changes required. Issues #12–#14 remain open.

Lichess broad PR #3 review `4848125095` remains changes required. Continue issue order #4 exact identity, #5 raw-byte UCI/options, #6 lifecycle/status, #7 SQLite.

## Immediate continuation

1. Resolve the pending PR #63 state-aware continue acknowledgement without duplicate commands.
2. Re-audit surviving Jules-derived code in risk order: Engine issue #64, Manager issue #81, Manager PR #73, Engine PR #52, Manager PRs #78→#80, then substantive PR #63 heads.
3. Keep queue issues #2–#4 blocked while control issue #1 remains open, active, or unresolved.
4. Reconstruct Engine PR #39 → #44 → #47 before replaying child slices.
5. Correct Manager PR #78 → #79 → #80 before refreshing issue #70.
6. Reconstruct Installer PR #3/#4, replay #5, then correct #11.
7. Trace every future replay/cherry-pick to its source and surviving exact lines.
8. Synchronize narrative, YAML, registry, retrospective ledger, and Installer PR #2 after each terminal review/control transition.

## Final gates

No soundness decision until each repository has one authoritative ancestry and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge cycles pass.
