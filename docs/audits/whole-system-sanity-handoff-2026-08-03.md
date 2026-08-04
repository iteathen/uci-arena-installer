# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot: `2026-08-04T07:05:14Z`  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2

This file, `whole-system-sanity-handoff-2026-08-03.yaml`, and `jules-workbenches/registry.yaml` are the continuation authority. Fetch current GitHub heads and Jules state before acting.

## Governing method

- Branch ancestry is product correctness.
- One owner per concern and project-owned, provider-neutral LEGO contracts at boundaries.
- Apply Domain-Appropriate Foundations rather than preserving accidental first-case limits.
- Require exact bounds for bytes, strings, lists, memory, paths, identities, timestamps, deadlines, retries, diagnostics, cleanup, and failure causality.
- Preserve the primary failure and attach bounded cleanup failures.
- Treat Jules output, PR descriptions, self-reviews, test names, and test-count claims as hostile input.
- Automatic CI is disabled by policy. Source review and isolated probes do not establish Node 24.15, Windows/Linux native, packaged, or release readiness.

## Whole-system conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness conclusion exists. Release readiness is not established. There is no valid authoritative Engine PR chain, and the Evidence stack is rejected from PR #10 upward.

## Engine

There is no authoritative Engine ancestry.

- PR #39 — head `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`, review `4850912840`, foundation reconstruction and split required. Reusable slices: concrete ONNX include removal and rejection of OS-owned Windows UCRT/API-set payloads. Audit: `engine-pr39-evaluation-release-hostile-review-2026-08-03.md`.
- PR #44 — head `d37ab900486cad25b5dff5cd6fa11724c8fc3929`, review `4850859460`, model-adapter foundation redesign; blocked parent. Audit: `engine-pr44-model-adapter-hostile-review-2026-08-03.md`.
- PR #47 — head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`, review `4850880052`, governance/heartbeat reconstruction; blocked parent. Audit: `engine-pr47-governance-heartbeat-hostile-review-2026-08-03.md`.
- PR #52 — head `5d122ac47ca80606add8f8354adabb67dbc4a2ff`, review `4850845427`, changes required; blocked parent. Audit: `engine-pr52-canonical-fen-hostile-review-2026-08-03.md`.
- PR #51 — review `4843786909`, rejected stale/noncompiling duplicate TT with no lock-free proof.
- PR #55 — review `4847014331`, rejected planning notes rather than implementation. No surviving authoritative lock-free TT exists.

### Engine issue #62 / PR #63 Jules workspace

- Control repository/issue: `iteathen/Jules-control#1`
- Session: `sessions/13034909149691235140`
- Starting branch/SHA: `agent/integrate-engine-audit-stacks` / `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- Output PR/branch: Engine #63 / `jules-13034909149691235140-b7c89938`
- Current reviewed head: `e7eb5edfbb584aff55e2414b36d769e0b4405fc0`
- Previous substantive head: `2106482635969e1e9c6b28b76fb80aca35890af6`
- Sixth formal hostile review: `4851511234`
- Sixth audit: `docs/audits/engine-pr63-resource-lease-sixth-hostile-review-2026-08-03.md`
- Sixth audit commit: `7dcdf4acef6b4f6258fcb14dfdaf08736ebd077b`
- Label/disposition: `changes-required`; replayable package-owned slice only; keep draft.

The session reported `COMPLETED` at `2026-08-04T06:53:25.552460Z`. The new head was substantive: two commits beyond `210648263…`, changing `background-resource-lease.mjs`, `lease-store.mjs`, and the focused revocation test.

Useful improvements in the head: private cache/waiter state, fatal UTF-8 plus descriptor checks for bounded reads, removal of the prior rename/restore deletion implementation, and promise continuations assigned before notifier construction.

Decisive remaining blockers:

- no persisted safe-integer original visibility authority;
- malformed/missing/future/ambiguous time rewritten to filesystem birth/ctime and emitted as trusted demand;
- future heartbeat tolerance can create negative elapsed time;
- no real packet/result/failure/package migration; package remains `1.0.2`;
- reason algebra exceeds the required bounded set;
- path-based heartbeat truncate/create and close-then-path-unlink races remain;
- no held ancestor identity or Windows junction/reparse-safe authority;
- release and cleanup failures are swallowed and released success can be reported while the holder remains;
- synchronous notifier callback-before-return can leak timer/watcher handles; a narrow Node 22 probe resolved successfully with one timer still active;
- time/path/work/cancellation/free-space bounds and metrics remain incomplete or false;
- equal-time ordering remains locale-sensitive;
- private root/filename state still crosses public surfaces;
- failure behavior remains inconsistent;
- tests encode timestamp invention and omit decisive exact-object/native races;
- PR body still overclaims completion.

Correction comment `5175680936` bound formal review `4851511234` into the existing session. State-aware continue command `5175682230` was acknowledged by `5175685564`: observed state `COMPLETED`, existing PR bound, latest correction replayed. Post-replay status command: `5175705079`. Do not send another correction or continue command while the resumed session is active. Review another head only if it moves beyond `e7eb5edf…` with substantive file changes.

Engine reconstruction order remains PR #39 foundations → PR #44 model adapter → PR #47 governance/heartbeat → replay corrected PR #52 and PR #63 package slices.

## Jules durable queue

Controller skips every task carrying a label beginning `blocked-`. It may launch one oldest eligible task only after all earlier outputs are terminally reviewed, the earlier control issue is closed, no session is active/waiting, budget permits, and no task-specific blocking label exists.

1. `Jules-control#2` — Evidence issue #20 / PR #23, starting head `3991983c397a222871cba70c6232da1a8be5b265`, review `4850754712` bound. Must not launch until Engine control issue #1 is terminally reviewed and closed.
2. `Jules-control#3` — Manager issue #70, label `blocked-base-review`. Must not launch until PRs #78/#79/#80 are corrected and the starting branch is refreshed.
3. `Jules-control#4` — Book Forge issue #12. Isolated exact-credential child only; cannot imply acceptance of blocked base PR #11.

## Manager

Authoritative base remains PR #54, branch `agent/release-assembly`, SHA `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

- PR #78 — head `9e18b058b4870b02f65b08fadb8d6f14df85d97b`, review `4850726806`, changes required.
- PR #79 — head `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`, review `4850708644`, changes required.
- PR #80 — head `683d7ae20a161d0b40d176d82173a87a961c77ea`, review `4850674462`, changes required.
- PR #67 — reviews `4847815394` and `4848995624`, reconstruction required.
- LocalAuth issue #69 remains open.

## Installer

Canonical base is PR #2 / `agent/release-assembly`.

- PR #3 — head `05776766064a014836650d46179eadffb73fed2a`, review `4850804908`, changes required; stale ancestry.
- PR #4 — head `cd061f11afbc9997710eb2f33c090311c5e4a485`, review `4850781709`, changes required; stale ancestry.
- PR #5 — head `16fe257ce6d55714a4d87efcea4ffe1314f4a2b7`, review `4850792446`, narrow action-enum/parser slice locally acceptable but parent/native blocked.
- PR #11 — head `c40edfcbe12a5e5187a4b1543aee610f40d47f02`, review `4850690366`, changes required.
- Issues #7–#10 remain open for exact stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

## Evidence Service

No authoritative Evidence stack exists.

- PR #23 — head `3991983c397a222871cba70c6232da1a8be5b265`, review `4850754712`, changes required; queued correction.
- PR #22 — head `3e9d9609382a3706aa82db66bca51096ea0dc0b1`, review `4850984541`, narrow validation slice locally acceptable but blocked by PR #19 and supported-runtime evidence.
- PR #19 — head `6b4988066751b40b7231597b2b19a2177d4d2284`, review `4850992135`, changes required.
- PR #18 — head `117cf64976fe179989bddee8dbfc534966370ec6`, review `4851001435`, changes required.
- PR #17 — head `640545739abe8db15d147c0202dabc684281b16f`, review `4851009132`, changes required.
- PR #16 — head `7dfc4388f5ce57105e8de515fdcb0526e9298df0`, review `4851046968`, changes required. Audit: `evidence-pr16-transactional-prune-hostile-review-2026-08-03.md`.
- PR #15 — head `a66fece5f8096622a53784479fe2c8da97fbc4f9`, review `4851061354`, changes required. Audit: `evidence-pr15-dataset-show-hostile-review-2026-08-03.md`.
- PR #14 — head `fbb1a9aa8d0ae1a2ad14be6a00d3bb229285a173`, review `4851124960`, reconstruction required. Audit commit `2b4ad1aa44ef49082e3d4e11e133f542f29099cf`.
- PR #13 — head `7b5e7f90476e17e5157f4a782a27122aa210a90b`, review `4851148254`, reconstruction required. Audit commit `7173555e3562f31ddf58bbcb57c48092e992665b`.
- PR #10 — head `b09abb9aecf6aba6ca04358c8789eef86ecb763b`, review `4851184668`, split/reconstruct by owner. Audit commit `a39c5d31eeda3cd824ba16f6158657467f9e8681`.

Issue #21 remains blocked on a corrected/replayed Engine resource-lease contract and requires one absolute 2000 ms deadline from earliest foreground visibility through verified complete child-tree termination and lease release.

## Book Forge and Lichess

Book Forge PR #10 review `4848065237` and PR #11 review `4848083104` remain changes required. Issues #12–#14 remain open.

Lichess broad PR #3 review `4848125095` remains changes required. Continue issue order: #4 exact file/publication/launch identity, #5 raw-byte UCI and typed options, #6 transactional lifecycle/status, #7 exact SQLite foundation.

## Immediate continuation

1. Check the response to status command `5175705079` before any further Jules command.
2. If resumed and active, do not interrupt it. Review a future head only when it moves substantively beyond `e7eb5edf…`.
3. Keep queue issues #2–#4 from launching while control issue #1 is open/active.
4. Continue independent ownership-sized reviews and reconstruction work.
5. Rebuild Engine authoritative ancestry in PR #39 → #44 → #47 ownership order before replaying child slices.
6. Correct Manager PR #78 → #79 → #80 before refreshing issue #70.
7. Reconstruct Installer PR #3/#4, replay #5, then correct #11.
8. Keep this narrative, its YAML companion, the registry, and Installer PR #2 discoverability synchronized after every terminal review or control transition.

## Final gates

No soundness decision until each repository has one authoritative ancestry and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge cycles pass.
