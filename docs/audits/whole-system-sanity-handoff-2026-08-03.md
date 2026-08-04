# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot time: 2026-08-03 22:30 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the canonical continuation state. Fetch current heads before acting.

## Governing rules

- One authority per concern; project-owned neutral LEGO contracts at boundaries.
- Branch ancestry is product correctness.
- Exact bounded bytes, identities, schemas, paths, lifetimes, deadlines, cleanup, and failure causality.
- No peer-source imports, hidden discovery, framework/private type leakage, or provider-private ownership crossing repositories.
- Jules output, self-review, test names, and completion claims are hostile input until independently reviewed line by line.
- Automatic CI remains disabled. Source review and isolated probes do not establish native, package, or release readiness.

## Current conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness or release-readiness conclusion may be issued.

## Engine — no authoritative PR chain currently exists

The previously recorded chain PR #39 → #44 → #47 is rejected at every foundation layer.

### PR #39 — evaluation and release foundations

- head `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`
- review `4850912840`
- disposition: **changes required; foundation reconstruction; split required**
- critical defects include fixed-array overflow in ordering reset, cache capacity violating its own memory budget, unsafe raw async lifetimes, callbacks under cache mutex, caller-held cache locks, accidental public capacities, weak toolchain/source/build identity, and mixed ownership across 36 files
- reusable sub-slices: concrete ONNX include removal and OS-owned UCRT/API-set exclusion
- audit: `docs/audits/engine-pr39-evaluation-release-hostile-review-2026-08-03.md`

### PR #44 — model-adapter foundation

- head `d37ab900486cad25b5dff5cd6fa11724c8fc3929`
- review `4850859460`
- disposition: **changes required; foundation redesign; blocked parent**
- hardcodes two preparation slots, four-axis view, int32/float32, mutable void pointer, adapter-owned batch buffers, raw artifact paths, weak policy/value types, and unbounded batch sizing
- audit: `docs/audits/engine-pr44-model-adapter-hostile-review-2026-08-03.md`

### PR #47 — governance and heartbeat integration

- head `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- review `4850880052`
- disposition: **changes required; governance reconstruction; blocked parent**
- spec registry deauthorizes contracts still required by preflight; no complete migration register
- heartbeat suppresses success but does not cancel work after authority loss, uses unversioned strings, leaks timers, has weak causality/timing bounds, and adds public API under unchanged package version
- audit: `docs/audits/engine-pr47-governance-heartbeat-hostile-review-2026-08-03.md`

### PR #52 — canonical FEN child

- head `5d122ac47ca80606add8f8354adabb67dbc4a2ff`
- review `4850845427`
- disposition: **changes required; blocked parent**
- preserves the correct early-gate/single-parser architecture but accepts adjacent empty-run digits, exposes the full validator across the source include tree, lacks a FEN byte bound, and leaves canonical numeric/castling text unresolved
- audit: `docs/audits/engine-pr52-canonical-fen-hostile-review-2026-08-03.md`

### Active Jules PR #63

- control `iteathen/Jules-control#1`
- session `sessions/13034909149691235140`
- output branch `jules-13034909149691235140-b7c89938`
- current reviewed head `cc2ead92a8abdb5d80a52072e5f0116aea34a33b`
- formal review `4850334357`
- state: genuinely `IN_PROGRESS` as of `2026-08-04T05:25:57Z`
- disposition: **changes required; blocked parent; replayable package slice only**
- Jules was instructed not to broaden into parent repair or claim merge readiness; comment `5174933429`

The current Engine reconstruction order is: split/correct PR #39 foundations → redesign PR #44 → reconcile PR #47 governance/heartbeat → replay corrected child slices PR #52 and PR #63.

## Jules capacity and durable queue

- free-account guard: 15 sessions per rolling 24 hours
- last API observation: 10 used, one active, five estimated remaining
- capacity is a scheduling constraint, never a reason to stop independent work

Queue:

1. `Jules-control#2` — Evidence #20 Catalog heartbeat projection; review `4850754712` bound into the prompt; eligible only after Engine control issue #1 closes.
2. `Jules-control#3` — Manager #70 verified launch; labeled `blocked-base-review`; must not launch until PRs #78–#80 are corrected and the exact branch is refreshed.
3. `Jules-control#4` — Book Forge #12 exact credential object; isolated child only and cannot imply acceptance of blocked base PR #11.

The hourly queue watcher skips `blocked-*` issues and revalidates dependency, review, concurrency, and budget gates.

## Installer

Authoritative base: PR #2.

- PR #3 head `05776766…`, review `4850804908`: changes required, stale ancestry—nondeterministic capped subset, duplicate underfill, false truncation, undocumented kind starvation, unenforceable Windows timeout, partial-result authority gaps.
- PR #4 head `cd061f11…`, review `4850781709`: changes required, stale ancestry—unsafe natural-exit reuse, nontransactional reader startup, unbounded failed termination, nonexact inheritance, unverified Windows cleanup, partial writes, byte-unbounded/lossy I/O.
- PR #5 head `16fe257c…`, review `4850792446`: local action enum/parser slice acceptable, blocked by PR #4 parent and native validation.
- PR #11 head `c40edfcb…`, review `4850690366`: changes required—platform vocabulary in neutral header, reset deadlines, failed-start retry, state loss, SCM infrastructure conflation, Linux nonblocking failure, divergent timeouts.

Issues #7–#10 still block stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

## Manager

Authoritative base remains PR #54 only; its child stack is rejected.

- PR #78 head `9e18b058…`, review `4850726806`: option-name `value` delimiter, string override edge spaces, untracked allocation evidence, incomplete exact bounds.
- PR #79 head `0cee8727…`, review `4850708644`: ambiguous completion tokens, stale pre-option identity, conflicting identity overwrite, locale-sensitive provider grammar, exception-prose matching, missing integration spec.
- PR #80 head `683d7ae2…`, review `4850674462`: Linux SIGPIPE termination, unsafe natural-exit reuse, pressure-dependent malformed-info behavior, duplicated wire authority, UCI policy in generic port, Windows stop hang, incomplete bounds/tests.
- PR #67 reviews `4847815394` and `4848995624`: storage reconstruction required.
- LocalAuth issue #69 remains open.

Queue issue #3 remains blocked until PRs #78–#80 are corrected and its starting branch is refreshed.

## Evidence Service

Authoritative stack ends at PR #22.

PR #23 head `3991983c…`, review `4850754712`, remains a changes-required partial: false Catalog-owner spec, unsafe clock domain, permissive timestamp parsing, public threshold override, duplicate wrapper clocks, malformed shape hiding, and no direct Node 24.15 Catalog/CLI/API evidence. Queue issue #2 carries the exact correction criteria.

Issue #21 remains blocked on a corrected/replayed Engine shared-runtime contract and requires one absolute 2000 ms deadline from earliest foreground visibility through verified complete child-tree termination and lease release.

## Book Forge

- PR #11 review `4848083104`: path/config/credential authority blocked.
- PR #10 review `4848065237`: transactional lock/failure causality blocked.
- queue issue #4 addresses only exact credential-object use.
- issues #13/#14 still require recoverable publication and exact catalog/64-shard SQLite authority.

## Lichess Bot

Broad PR #3 review `4848125095` remains changes required; aggregate test count is not acceptance evidence. Continue issues #4 exact identity, #5 UCI/options, #6 lifecycle/status, #7 SQLite in ownership order.

## Immediate continuation order

1. Review PR #63 only when its head moves beyond `cc2ead92…`; do not continue while it remains genuinely active.
2. Continue independent provider/Installer/Manager review and ownership-sized direct repairs.
3. Reconstruct Engine from split PR #39 foundations, then PR #44, then PR #47; child work is replay evidence only.
4. Reconstruct Installer PR #3/#4 on current PR #2, replay PR #5, then correct PR #11.
5. Correct Manager PR #78 → #79 → #80, then refresh/unblock queue issue #3.
6. Launch Evidence queue #2 only after Engine control issue #1 reaches terminal review and closes.
7. Continue Evidence #21, Manager #67/#69, Lichess #4–#7, Book Forge #12–#14, Installer #7–#10.
8. Keep narrative, YAML, registry, and PR #2 discoverability synchronized.

## Final gates

No soundness decision until authoritative ancestry is reconstructed and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge lifecycle cycles pass.
