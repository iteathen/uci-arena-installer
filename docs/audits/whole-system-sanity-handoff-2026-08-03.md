# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot: after Engine PR #63 seventh hostile review on 2026-08-04  
Canonical repository/branch: `iteathen/uci-arena-installer` / `agent/release-assembly` (Installer PR #2)

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the continuation authority. Fetch current GitHub heads and Jules state before acting.

## Governing method

- Branch ancestry is product correctness.
- Use one owner per concern and project-owned, provider-neutral LEGO contracts.
- Apply Domain-Appropriate Foundations rather than accidental first-case limits.
- Require exact bounds for bytes, strings, lists, memory, paths, identities, timestamps, deadlines, retries, diagnostics, cleanup, and failure causality.
- Preserve primary failure and attach bounded cleanup failures.
- Treat Jules output, PR prose, self-review, test names, and test-count claims as hostile input.
- Automatic CI is disabled. Source review and isolated probes do not establish Node 24.15, Windows/Linux native, packaged, or release readiness.

## Whole-system conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness conclusion exists. Release readiness is not established. There is no authoritative Engine PR chain. The Evidence stack is rejected from PR #10 upward.

## Engine

No authoritative ancestry exists.

- PR #39 — head `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`, review `4850912840`, foundation reconstruction/split required.
- PR #44 — head `d37ab900486cad25b5dff5cd6fa11724c8fc3929`, review `4850859460`, model-adapter redesign; blocked parent.
- PR #47 — head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`, review `4850880052`, governance/heartbeat reconstruction; blocked parent.
- PR #52 — head `5d122ac47ca80606add8f8354adabb67dbc4a2ff`, review `4850845427`, changes required; blocked parent.
- PR #51 review `4843786909` and PR #55 review `4847014331` remain rejected. No surviving authoritative lock-free TT exists.

### Jules issue #62 / Engine PR #63

- Control: `iteathen/Jules-control#1`
- Session: `sessions/13034909149691235140`
- Starting branch/SHA: `agent/integrate-engine-audit-stacks` / `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- Output branch: `jules-13034909149691235140-b7c89938`
- Exact reviewed head: `ebf510670da82a22ca55626c8e520fd1af494d6f`
- Seventh formal review: `4852208692`
- Seventh audit: `docs/audits/engine-pr63-resource-lease-seventh-hostile-review-2026-08-04.md`
- Seventh audit commit: `67cbd8126fc3dfcbbf16d9595b3ff5179b349ad7`
- Disposition: **rejected; changes required; keep draft; replayable package-owned slice only**.

Useful partial changes at this head include bounded descriptor reads, fatal UTF-8, private instance cache/waiters, deterministic non-locale ordering, a package bump to 1.0.3, and a persisted `requested_at_ms` field. Jules's commit/PR claim that the secure redesign and all hostile-review corrections are complete is false.

Confirmed blockers:

1. `safeUnlink()` closes the verified descriptor before pathname unlink and can delete a replacement object; a narrow local Node 22 probe reproduced the race.
2. Request heartbeat still opens/truncates/recreates by mutable pathname, ignores partial `writeSync`, and adopts replacement identity afterward.
3. Holder publication closes the created object and reopens the mutable name in the constructor.
4. In-place holder truncate/write permits a concurrent reader to classify transient bytes as malformed and delete the active holder.
5. Release closes exact authority before deletion, swallows close failure, uses the unsafe unlink, and performs a racy `existsSync` check.
6. Release can replace a primary work failure; acquisition/success cleanup failures are swallowed; constructor failure can strand authority.
7. `requested_at_ms` is not required, checked, or bound to canonical text and is ignored during selection.
8. The 1000 ms future tolerance still clamps producer time to observation time.
9. A stale foreground request with a live PID becomes permanent phantom `untrusted-foreground` demand; the named stale test has no stale case.
10. Package/schema/result/failure and consumer-lock migration is incomplete; Evidence pins 1.0.2 and Book Forge pins 1.0.0.
11. Public authority leaks remain: exported global test seam, publicly writable `released`, and snapshot filename/path leakage.
12. Synchronous terminal notifier callbacks before constructor return can leak watcher/timer handles; the new test invokes a nonterminal callback and misses the race. A narrow probe reproduced the leak.
13. Waiter/watcher/timer count and lifetime are unbounded; success cleanup failures disappear.
14. Windows protection is pathname check-then-use rather than retained handle/file-ID authority; the test is an ordinary symlink and silently returns if creation fails.
15. The 1 MiB byte metric excludes invalid reads; up to roughly 12.8 MiB can be read under the stated bound; allocation metrics are manual and all traversal errors become overflow.
16. `observe()` accepts safe integers outside Date range and still uses multiple unsynchronized clocks.
17. Failure semantics remain inconsistent across null, booleans, strings, plain errors, typed errors, and conservative observations.
18. The active spec and PR body materially overclaim the mechanisms and evidence; the head has no CI status evidence.

Independent probes were Node 22 falsification probes only. They do not establish Node 24.15, Windows, Linux, package, or release readiness.

Control state:

- Jules terminal status before review: `COMPLETED` at `2026-08-04T07:46:56.532963Z`, status comment `5176310334`.
- Seventh correction comment: `5176539894`.
- State-aware continue command: `5176541573`.
- Controller acknowledgement/status after this command was not yet present in the fetched timeline when this handoff was written.

Do not send another correction/continue until the controller resolves the pending command. If it resumes, do not interrupt it. Review only a substantive head beyond `ebf510670…`.

Engine reconstruction order remains PR #39 foundations → PR #44 model adapter → PR #47 governance/heartbeat → replay corrected PR #52 and PR #63 child slices.

## Jules durable queue

The controller skips labels beginning `blocked-` and may launch only one oldest eligible task after all earlier outputs are terminally reviewed, the earlier control issue is closed, no session is active/waiting, budget permits, and no task-specific block exists.

1. `Jules-control#2` — Evidence issue #20 / PR #23; review `4850754712` bound. Blocked until control issue #1 is terminally reviewed and closed.
2. `Jules-control#3` — Manager issue #70; `blocked-base-review`. Blocked until PRs #78/#79/#80 are corrected and the branch is refreshed.
3. `Jules-control#4` — Book Forge issue #12. Isolated credential child only; cannot imply acceptance of blocked PR #11.

## Manager

Authoritative base remains PR #54 at `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

- PR #78 review `4850726806`, changes required.
- PR #79 review `4850708644`, changes required.
- PR #80 review `4850674462`, changes required.
- PR #67 reviews `4847815394` and `4848995624`, reconstruction required.
- LocalAuth issue #69 remains open.

## Installer

Canonical base is PR #2.

- PR #3 review `4850804908`, changes required; stale ancestry.
- PR #4 review `4850781709`, changes required; stale ancestry.
- PR #5 review `4850792446`, narrow slice locally acceptable but parent/native blocked.
- PR #11 review `4850690366`, changes required.
- Issues #7–#10 remain open.
- Accidental connector issue #12 was immediately marked `not_planned`, closed, and explicitly has no project or audit authority.

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

1. Resolve the pending state-aware Jules command without sending duplicates.
2. Review only a substantive PR #63 head beyond `ebf510670…`.
3. Keep queue issues #2–#4 from launching while control issue #1 remains open, active, or unresolved.
4. Continue independent ownership-sized reviews and reconstruction.
5. Reconstruct Engine PR #39 → #44 → #47 before replaying child slices.
6. Correct Manager PR #78 → #79 → #80 before refreshing issue #70.
7. Reconstruct Installer PR #3/#4, replay #5, then correct #11.
8. Synchronize narrative, YAML, registry, and Installer PR #2 after each terminal review/control transition.

## Final gates

No soundness decision until each repository has one authoritative ancestry and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge cycles pass.
