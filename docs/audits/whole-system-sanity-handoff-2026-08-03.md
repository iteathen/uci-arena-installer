# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot time: 2026-08-03 23:19 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the canonical continuation state. Fetch current heads and session state before acting.

## Governing rules

- One authority per concern; project-owned provider-neutral LEGO contracts at boundaries.
- Branch ancestry is product correctness.
- Apply Domain-Appropriate Foundations; do not preserve accidental limits from the first implementation.
- Require exact bounded bytes, strings, lists, memory, paths, identities, timestamps, deadlines, retries, diagnostics, cleanup, and failure causality.
- Preserve primary failure and attach bounded cleanup failures; never silently replace or discard causality.
- Jules output, PR prose, self-review, test names, and completion claims are hostile input until independently reviewed.
- Automatic CI remains disabled. Source review and isolated probes do not establish Node 24.15, Windows/Linux native, package, or release readiness.

## Current conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness or release-readiness conclusion may be issued. There is no authoritative Engine ancestry, and the previously presumed Evidence stack is rejected from its PR #10 foundation upward.

## Engine — no authoritative PR chain currently exists

The previously recorded PR #39 → #44 → #47 ancestry is rejected at every foundation layer.

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
- hardcodes two preparation slots, four-axis view, int32/float32, mutable `void*`, adapter-owned batch buffers, raw artifact paths, weak policy/value types, and unbounded batch sizing
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
- preserves the early-gate/single-parser architecture but accepts adjacent empty-run digits, exposes the validator too broadly, lacks a FEN byte bound, and leaves canonical numeric/castling text unresolved
- audit: `docs/audits/engine-pr52-canonical-fen-hostile-review-2026-08-03.md`

### Transposition table

- PR #51 review `4843786909`: stale, noncompiling, duplicate TT probe, no lock-free proof or memory/performance evidence.
- PR #55 review `4847014331`: planning notes rather than the requested implementation.
- no surviving authoritative lock-free TT implementation exists.

### Active Jules PR #63

- control `iteathen/Jules-control#1`
- session `sessions/13034909149691235140`
- starting branch `agent/integrate-engine-audit-stacks` at `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- output branch `jules-13034909149691235140-b7c89938`
- latest substantively reviewed head `2106482635969e1e9c6b28b76fb80aca35890af6`
- latest formal review `4850967412`
- latest audit: `docs/audits/engine-pr63-resource-lease-fifth-hostile-review-2026-08-03.md`
- correction comment `5175041945`; state-aware continue command `5175048852`; recovery acknowledgement `5175050145`
- current PR head `c7ef021e74d853ff6a5500d4d4396ef5e5d6b5ab`
- comparison from `210648263…` to `c7ef021e…`: one commit ahead, **zero changed files**; this is not a substantive correction and does not require or support a duplicate review
- current session state: genuinely `IN_PROGRESS`, last confirmed at `2026-08-04T06:18:53.275899Z`
- do not send another correction or continue command while the session is active
- if the session reaches terminal state with no substantive head beyond `210648263…`, replay the latest correction only through the existing state-aware control path
- disposition remains **changes required; blocked parent; replayable package-owned slice only**

The last substantive implementation still lacked persisted safe-integer visibility authority, preserved malformed/future timing as observation time, had no real package/result versioning, destructive replacement races, no Windows/ancestor reparse identity, public mutable cache/metrics/test seams, false bounds, locale-sensitive ordering, inconsistent failure algebra, wait/cleanup races, debug output, incomplete clock/producer bounds, and decisive native adversarial tests.

The Engine reconstruction order remains: split/correct PR #39 foundations → redesign PR #44 → reconcile PR #47 governance/heartbeat → replay corrected PR #52 and PR #63 slices.

## Jules capacity and durable queue

- configured guard: 15 sessions per rolling 24 hours
- last API observation: 10 used, one active, five estimated remaining; the API did not expose an authoritative account limit/reset field
- capacity is a scheduling constraint, never a reason to stop independent audit work

Queue:

1. `Jules-control#2` — Evidence issue #20 / PR #23 Catalog heartbeat correction; review `4850754712` bound into the prompt; eligible only after Engine control issue #1 is terminally reviewed and closed.
2. `Jules-control#3` — Manager issue #70 verified launch; labeled `blocked-base-review`; must not launch until PRs #78–#80 are corrected and the exact branch is refreshed.
3. `Jules-control#4` — Book Forge issue #12 exact credential object; isolated child only and cannot imply acceptance of blocked base PR #11.

The hourly watcher skips `blocked-*` issues and must revalidate dependency, terminal review, closed earlier issue, concurrency, budget, and task-specific blocking labels before launching one oldest eligible task.

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

## Evidence Service — no authoritative stack

The stack was reviewed from the top downward and is rejected from PR #10 upward. Narrow reusable shapes do not overcome blocked ancestry.

### PR #23 — daemon heartbeat projection

- head `3991983c397a222871cba70c6232da1a8be5b265`
- review `4850754712`
- disposition: **changes required; queued correction**
- false Catalog-owner specification, unsafe clock domain, permissive timestamps, public threshold override, duplicate wrappers, malformed daemon shape hidden as missing, and no direct Node 24.15 Catalog/CLI/API evidence

### PR #22 — resource validation child

- head `3e9d9609382a3706aa82db66bca51096ea0dc0b1`
- review `4850984541`
- disposition: **narrow integer/continuous/list validation locally acceptable; blocked by PR #19 and supported-runtime/full-suite evidence**

### PR #19 — timing preflight

- head `6b4988066751b40b7231597b2b19a2177d4d2284`
- review `4850992135`
- disposition: **changes required**
- actual daemon consumer missing from timing preflight, no operational timer tests, heartbeat failures swallowed, timer construction outside cleanup, mutable timing projection, parent/runtime evidence unresolved

### PR #18 — exact runtime identity

- head `117cf64976fe179989bddee8dbfc534966370ec6`
- review `4851001435`
- disposition: **changes required**
- verified paths later reused by mutable name, engine digest not bound to execution, no Windows/ancestor reparse safety, nonfatal UTF-8, string-coerced persisted paths, model fingerprint not bound to package bytes, unbounded hashing, weak diagnostics/dedup/preflight, missing use-after-validation tests

### PR #17 — branch topology integration

- head `640545739abe8db15d147c0202dabc684281b16f`
- review `4851009132`
- disposition: **changes required**
- topology integration is useful, but unsafe predecessor implementations and mutable legacy owner-root compatibility authority remain

### PR #16 — transactional prune

- head `7dfc4388f5ce57105e8de515fdcb0526e9298df0`
- review `4851046968`
- disposition: **changes required**
- transactional shape is useful, but exact object operations, Catalog admission, daemon/writer coordination, lease timing, cancellation, manifests, durability, recovery bounds, committed-result causality, and destructive tests are absent
- audit: `docs/audits/evidence-pr16-transactional-prune-hostile-review-2026-08-03.md`

### PR #15 — streamed dataset show

- head `a66fece5f8096622a53784479fe2c8da97fbc4f9`
- review `4851061354`
- disposition: **changes required**
- spool-before-output is useful, but validated bytes are reopened/deleted by mutable pathname, exports-root identity is untrusted, final rename can overwrite, cleanup is swallowed, stream lifecycle is incomplete, and exact successful output identity is absent
- audit: `docs/audits/evidence-pr15-dataset-show-hostile-review-2026-08-03.md`

### PR #14 — streamed dataset export

- head `fbb1a9aa8d0ae1a2ad14be6a00d3bb229285a173`
- review `4851124960`
- disposition: **changes required; reconstruction required**
- exact Catalog object/workspace/schema/integrity identity absent; stored evidence admission is JSON parse rather than strict schema; JSON/serialization unbounded; the live SQLite snapshot spans the full filesystem export and can pin WAL growth; physical aliases and reparse substitution bypass path checks; rename overwrites; temp cleanup/publication trust mutable pathnames; directory durability, cleanup causality, cancellation/free-space/work bounds, and exact bytes/digest result binding are absent
- audit: `docs/audits/evidence-pr14-dataset-export-hostile-review-2026-08-03.md`
- audit commit: `2b4ad1aa44ef49082e3d4e11e133f542f29099cf`

### PR #13 — CLI grammar

- head `7b5e7f90476e17e5157f4a782a27122aa210a90b`
- review `4851148254`
- disposition: **changes required; reconstruction required**
- duplicated command authority, mutable string/flag bags, generic accidental limits, lossy duplicate-key/unsafe-number JSON, copied domain enums, unversioned incompatible behavior, weak field grammars, unbounded diagnostics, bypassable helper surface, and self-referential tests
- audit: `docs/audits/evidence-pr13-cli-contract-hostile-review-2026-08-03.md`
- audit commit: `7173555e3562f31ddf58bbcb57c48092e992665b`

### PR #10 — system-sanity foundation

- head `b09abb9aecf6aba6ca04358c8789eef86ecb763b`
- review `4851184668`
- disposition: **changes required; split and reconstruct by owner**
- PR source/version claims conflict; vendor provenance is self-asserted and stale; vendor hashing is canonicalized rather than exact bytes; packaged Installer entrypoint imports omitted `vendor` content; untracked `node_modules` enters a supposedly clean source package; source-to-copy races, destructive staging/output publication, unbounded archive work, blacklist-only Windows closure, self-reported native toolchain, TOCTOU Installer/config identity, ineffective option validation, and false migration/capability contract claims remain
- audit: `docs/audits/evidence-pr10-system-sanity-hostile-review-2026-08-03.md`
- audit commit: `a39c5d31eeda3cd824ba16f6158657467f9e8681`

Issue #21 remains blocked on a corrected/replayed Engine shared-runtime contract and requires one absolute 2000 ms deadline from earliest foreground visibility through verified complete child-tree termination and lease release.

## Book Forge

- PR #11 review `4848083104`: path/config/credential authority blocked.
- PR #10 review `4848065237`: transactional lock/failure causality blocked.
- queue issue #4 addresses only exact credential-object use.
- issues #13/#14 still require recoverable publication and exact catalog/64-shard SQLite authority.

## Lichess Bot

Broad PR #3 review `4848125095` remains changes required; aggregate test count is not acceptance evidence. Continue issues #4 exact identity, #5 raw-byte UCI/options, #6 lifecycle/status, and #7 exact SQLite foundation in ownership order.

## Immediate continuation order

1. Keep Jules session `13034909149691235140` uninterrupted while it remains `IN_PROGRESS`; recheck state and PR #63 head before any correction or continue command.
2. If a new substantive PR #63 head appears, hostile-review only that delta. If the session becomes terminal with no substantive head beyond `210648263…`, replay the latest correction through the existing state-aware path.
3. Continue independent provider/Installer/Manager review and ownership-sized direct repairs.
4. Reconstruct Engine from split PR #39 foundations, then PR #44, then PR #47; child work remains replay evidence only.
5. Reconstruct Installer PR #3/#4 on current PR #2, replay PR #5, then correct PR #11.
6. Correct Manager PR #78 → #79 → #80, then refresh/unblock queue issue #3.
7. Launch Evidence queue #2 only after Engine control issue #1 is terminally reviewed and closed.
8. Continue Evidence #21, Manager #67/#69, Lichess #4–#7, Book Forge #12–#14, Installer #7–#10.
9. Keep narrative, YAML, registry, and PR #2 discoverability synchronized.

## Final gates

No soundness decision until authoritative ancestry is reconstructed and all specs/preflight/boundary/substitution checks, Windows/Linux native builds, Node 24.15 suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifacts/provenance, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge lifecycle cycles pass.
