# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot: `2026-08-04T07:04:44.292177Z`  
Canonical repository/branch: `iteathen/uci-arena-installer` / `agent/release-assembly` (Installer PR #2)

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the continuation authority. Fetch current GitHub heads and Jules state before acting.

## Governing method

- Branch ancestry is product correctness.
- Use one owner per concern and project-owned, provider-neutral LEGO contracts at boundaries.
- Apply Domain-Appropriate Foundations rather than accidental first-case limits.
- Require exact bounds for bytes, strings, lists, memory, paths, identities, timestamps, deadlines, retries, diagnostics, cleanup, and failure causality.
- Preserve the primary failure and attach bounded cleanup failures.
- Treat Jules output, PR prose, self-review, test names, and test-count claims as hostile input.
- Automatic CI is disabled. Source review and isolated probes do not establish Node 24.15, Windows/Linux native, packaged, or release readiness.

## Whole-system conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness conclusion exists. Release readiness is not established. There is no authoritative Engine PR chain. The Evidence stack is rejected from PR #10 upward.

## Engine

No authoritative ancestry exists.

- PR #39 — head `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`, review `4850912840`, foundation reconstruction and split required. Audit: `engine-pr39-evaluation-release-hostile-review-2026-08-03.md`.
- PR #44 — head `d37ab900486cad25b5dff5cd6fa11724c8fc3929`, review `4850859460`, model-adapter foundation redesign; blocked parent. Audit: `engine-pr44-model-adapter-hostile-review-2026-08-03.md`.
- PR #47 — head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`, review `4850880052`, governance/heartbeat reconstruction; blocked parent. Audit: `engine-pr47-governance-heartbeat-hostile-review-2026-08-03.md`.
- PR #52 — head `5d122ac47ca80606add8f8354adabb67dbc4a2ff`, review `4850845427`, changes required; blocked parent. Audit: `engine-pr52-canonical-fen-hostile-review-2026-08-03.md`.
- PR #51 review `4843786909` and PR #55 review `4847014331` remain rejected. No surviving authoritative lock-free TT implementation exists.

### Active Jules issue #62 / PR #63 correction

- Control: `iteathen/Jules-control#1`
- Session: `sessions/13034909149691235140`
- Starting branch/SHA: `agent/integrate-engine-audit-stacks` / `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- Output PR/branch: Engine #63 / `jules-13034909149691235140-b7c89938`
- Latest reviewed head: `e7eb5edfbb584aff55e2414b36d769e0b4405fc0`
- Sixth formal review: `4851511234`
- Sixth audit: `docs/audits/engine-pr63-resource-lease-sixth-hostile-review-2026-08-03.md`
- Audit commit: `7dcdf4acef6b4f6258fcb14dfdaf08736ebd077b`
- Disposition: **changes required; blocked parent; replayable package-owned slice only; keep draft**.

The completed head improved private cache/waiter encapsulation, fatal UTF-8 and descriptor checks, removed the previous rename/restore deletion implementation, and assigned promise continuations before notifier construction. It still failed because:

- safe-integer original visibility authority is not persisted;
- malformed/missing/future/ambiguous timing is rewritten to filesystem birth/ctime and trusted;
- future heartbeat tolerance can create negative elapsed time;
- packet/result/failure/package migration is absent and package remains `1.0.2`;
- reason algebra exceeds the required bounded set;
- holder/request updates and deletion remain mutable-path races;
- Windows and ancestor junction/reparse authority is absent;
- cleanup failures are swallowed and release may report success while a holder remains;
- synchronous notifier callback-before-return can leak a timer/watcher handle;
- path, time, work, cancellation, free-space, and metric bounds remain incomplete or false;
- ordering remains locale-sensitive;
- private lease paths still cross public surfaces;
- failure behavior remains inconsistent;
- decisive Node 24.15 Windows/Linux adversarial evidence is absent;
- the PR body remains stale and overclaims completion.

Control transitions:

- terminal status `COMPLETED` at `2026-08-04T06:53:25.552460Z`, comment `5175618975`;
- sixth correction comment `5175680936`;
- state-aware continue command `5175682230`;
- acknowledgement `5175685564`: existing PR bound and latest correction replayed;
- post-replay status command `5175705079`;
- post-replay status response `5175706829`: `IN_PROGRESS` at `2026-08-04T07:04:44.292177Z`.

**Do not send another correction or continue command while this session remains active.** Review another head only if it moves substantively beyond `e7eb5edf…`.

Engine reconstruction order remains PR #39 foundations → PR #44 model adapter → PR #47 governance/heartbeat → replay corrected PR #52 and PR #63 child slices.

## Jules durable queue

The controller skips labels beginning `blocked-` and may launch only one oldest eligible task after all earlier outputs are terminally reviewed, the earlier control issue is closed, no session is active/waiting, budget permits, and no task-specific block exists.

1. `Jules-control#2` — Evidence issue #20 / PR #23; review `4850754712` bound. Blocked until control issue #1 is terminally reviewed and closed.
2. `Jules-control#3` — Manager issue #70; label `blocked-base-review`. Blocked until PRs #78/#79/#80 are corrected and the branch is refreshed.
3. `Jules-control#4` — Book Forge issue #12. Isolated credential child only; cannot imply acceptance of blocked PR #11.

## Manager

Authoritative base remains PR #54 at `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

- PR #78 head `9e18b058b4870b02f65b08fadb8d6f14df85d97b`, review `4850726806`, changes required.
- PR #79 head `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`, review `4850708644`, changes required.
- PR #80 head `683d7ae20a161d0b40d176d82173a87a961c77ea`, review `4850674462`, changes required.
- PR #67 reviews `4847815394` and `4848995624`, reconstruction required.
- LocalAuth issue #69 remains open.

## Installer

Canonical base is PR #2.

- PR #3 review `4850804908`, changes required; stale ancestry.
- PR #4 review `4850781709`, changes required; stale ancestry.
- PR #5 review `4850792446`, narrow slice locally acceptable but parent/native blocked.
- PR #11 review `4850690366`, changes required.
- Issues #7–#10 remain open for exact stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

## Evidence Service

No authoritative Evidence stack exists.

- PR #23 review `4850754712`, changes required; queued correction.
- PR #22 review `4850984541`, narrow slice locally acceptable but blocked by PR #19 and supported-runtime evidence.
- PR #19 review `4850992135`, changes required.
- PR #18 review `4851001435`, changes required.
- PR #17 review `4851009132`, changes required.
- PR #16 review `4851046968`, changes required. Audit: `evidence-pr16-transactional-prune-hostile-review-2026-08-03.md`.
- PR #15 review `4851061354`, changes required. Audit: `evidence-pr15-dataset-show-hostile-review-2026-08-03.md`.
- PR #14 review `4851124960`, reconstruction required. Audit commit `2b4ad1aa44ef49082e3d4e11e133f542f29099cf`.
- PR #13 review `4851148254`, reconstruction required. Audit commit `7173555e3562f31ddf58bbcb57c48092e992665b`.
- PR #10 review `4851184668`, split/reconstruct by owner. Audit commit `a39c5d31eeda3cd824ba16f6158657467f9e8681`.

Issue #21 remains blocked on a corrected/replayed Engine resource-lease contract and requires one absolute 2000 ms deadline from earliest foreground visibility through verified complete child-tree termination and lease release.

## Book Forge and Lichess

Book Forge PR #10 review `4848065237` and PR #11 review `4848083104` remain changes required. Issues #12–#14 remain open.

Lichess broad PR #3 review `4848125095` remains changes required. Continue issue order: #4 exact file/publication/launch identity, #5 raw-byte UCI and typed options, #6 transactional lifecycle/status, #7 exact SQLite foundation.

## Immediate continuation

1. Leave the active Jules session uninterrupted.
2. Review only a substantive PR #63 head beyond `e7eb5edf…`.
3. Keep queue issues #2–#4 from launching while control issue #1 remains open/active.
4. Continue independent ownership-sized reviews and reconstruction.
5. Reconstruct Engine PR #39 → #44 → #47 before replaying child slices.
6. Correct Manager PR #78 → #79 → #80 before refreshing issue #70.
7. Reconstruct Installer PR #3/#4, replay #5, then correct #11.
8. Synchronize this narrative, YAML, registry, and Installer PR #2 discoverability after each terminal review/control transition.

## Final gates

No soundness decision until each repository has one authoritative ancestry and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge cycles pass.
