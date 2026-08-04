# Whole-system sanity audit handoff — 2026-08-03

Status: **in progress; no soundness or release-readiness conclusion exists**  
Snapshot time: 2026-08-03 22:10 America/Los_Angeles  
Canonical repository: `iteathen/uci-arena-installer`  
Canonical branch: `agent/release-assembly` / Installer PR #2

This file, its YAML companion, and `docs/audits/jules-workbenches/registry.yaml` are the canonical continuation state. Fetch every current head before trusting a recorded SHA.

## Governing rules

- One authority per concern; project-owned neutral LEGO contracts at boundaries.
- Branch ancestry is product correctness.
- Exact bounded bytes, identities, schemas, paths, lifetimes, deadlines, cleanup, and failure causality.
- No peer-source imports, hidden discovery, framework/private type leakage, or provider-private ownership crossing repositories.
- Jules output, self-review, test names, and completion claims are hostile input until independently reviewed line by line.
- Automatic CI remains disabled. Source review and isolated probes do not establish native, package, or release readiness.

## Current conclusion

The six-repository audit remains incomplete. `PKG-E2E-011` is unknown. No soundness or release-readiness conclusion may be issued.

## Active Jules session and durable queue

Engine issue #62 / PR #63 remains the only active Jules task.

- control: `iteathen/Jules-control#1`
- session: `sessions/13034909149691235140`
- API starting branch: `agent/integrate-engine-audit-stacks`
- starting SHA: `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`
- output PR: Engine #63, draft, correct base
- output branch: `jules-13034909149691235140-b7c89938`
- current reviewed head: `cc2ead92a8abdb5d80a52072e5f0116aea34a33b`
- formal hostile review: `4850334357`
- state: architectural correction genuinely `IN_PROGRESS`; do not issue another continue unless it stops without a new result

The current implementation still lacks a versioned public lease contract, trustworthy persisted visibility milliseconds, exact cross-platform object identity, race-free update/delete, bounded notifier/index behavior, consistent typed failure semantics, private test seams, truthful specs, and Windows/Linux Node 24.15 evidence.

Jules free-account guard: 15 sessions per rolling 24 hours. Last API observation: 10 used, one active, five estimated remaining. Capacity is a scheduling constraint, never a reason to stop independent audit work.

Durable queue:

1. `Jules-control#2` — Evidence #20 Catalog heartbeat projection; review `4850754712` is bound into the task prompt; next eligible after Engine #62 is terminally reviewed and control issue #1 closes.
2. `Jules-control#3` — Manager #70 exact verified launch identity; labeled `blocked-base-review`; must not launch until Manager PRs #78–#80 are corrected and the exact starting branch is refreshed.
3. `Jules-control#4` — Book Forge #12 exact credential-object use; may produce only an isolated child correction and cannot imply acceptance of blocked base PR #11.

The hourly queue watcher skips `blocked-*` issues and revalidates review closure, concurrency, dependency, and rolling-budget gates before `/jules launch`.

## Installer

Authoritative base: PR #2 `agent/release-assembly`.

### PR #3 — indexed asset bound

- head `05776766064a014836650d46179eadffb73fed2a`
- formal review `4850804908`
- disposition: **changes required; stale ancestry; mergeable=false**
- confirmed failures: nondeterministic capped subset, duplicates consuming capacity before dedup, false truncation, undocumented cross-kind starvation, unenforceable Windows timeout, partial candidates on failure, Windows path-identity mismatch, helper-only tests
- audit: `docs/audits/installer-pr3-indexed-asset-hostile-review-2026-08-03.md`

### PR #4 — native process tree

- head `cd061f11afbc9997710eb2f33c090311c5e4a485`
- formal review `4850781709`
- disposition: **changes required; stale ancestry; mergeable=false**
- confirmed failures: natural-exit restart can terminate process, reader-thread construction leaks live child, unbounded reap after failed termination, non-exact descriptor/handle inheritance, unverified startup cleanup, potentially unbounded Windows reader join, partial Windows writes, byte-unbounded/lossy I/O
- audit: `docs/audits/installer-pr4-native-process-hostile-review-2026-08-03.md`

### PR #5 — typed service actions

- head `16fe257ce6d55714a4d87efcea4ffe1314f4a2b7`
- formal review `4850792446`
- disposition: **local five-file action-vocabulary slice acceptable; blocked-parent**
- retain the exact enum/parser design when rebuilding; do not merge until corrected PR #4 ancestry and full native validation exist

### PR #11 — platform-neutral service result

- head `c40edfcbe12a5e5187a4b1543aee610f40d47f02`
- formal review `4850690366`
- disposition: **changes required**
- confirmed failures: systemd/Win32 vocabulary leaked into neutral header, reset Windows deadlines, failed-start retry, loss of observed state, SCM infrastructure/target-state conflation, ignored Linux nonblocking setup failure, divergent timeout state semantics, missing production/native tests
- audit: `docs/audits/installer-pr11-service-control-hostile-review-2026-08-03.md`

Issues #7–#10 still block package/lifecycle conclusions: exact stable-byte admission, bounded acquisition/archive expansion, recoverable apply/rollback, and exact uninstall/purge authority.

## Manager

Authoritative base: PR #54 `agent/release-assembly`, SHA `233654568e91d50eb5e0a995fd22e109c3d2a66d`.

### PR #78 — pure UCI option contract

- head `9e18b058b4870b02f65b08fadb8d6f14df85d97b`
- formal review `4850726806`
- disposition: **changes required**
- pure boundary is otherwise strong, but option names can contain the `value` command delimiter, string overrides allow ambiguous edge spaces, allocation evidence is not tracked/repeatable, and exact boundary tests are incomplete
- audit: `docs/audits/manager-pr78-uci-option-contract-hostile-review-2026-08-03.md`

### PR #79 — process adoption

- head `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`
- formal review `4850708644`
- disposition: **changes required**
- accepts ambiguous `uciok`/`readyok`, can reuse stale pre-option runtime identity, overwrites conflicting identity, uses locale-sensitive provider grammar, matches exception prose for compatibility, and lacks a governing integration spec
- audit: `docs/audits/manager-pr79-uci-integration-hostile-review-2026-08-03.md`

### PR #80 — raw wire framing

- head `683d7ae20a161d0b40d176d82173a87a961c77ea`
- formal review `4850674462`
- disposition: **changes required**
- confirmed failures: Linux SIGPIPE can terminate Manager, natural-exit restart can invoke terminate on both platforms, malformed `info` validity depends on queue pressure, duplicated wire/limit authorities, UCI policy leaks into generic process port, Windows stop can hang, timeout bounds and adversarial tests incomplete
- audit: `docs/audits/manager-pr80-wire-framing-hostile-review-2026-08-03.md`

Manager PR #67 remains under reconstruction reviews `4847815394` and `4848995624`: exact SQLite storage classes, path/open object identity, verified backup-before-migration, canonical schema classification/integrity, and deterministic failure recovery are absent. LocalAuth issue #69 remains open.

The Manager verified-launch Jules task is blocked until PRs #78–#80 are corrected and its exact starting branch is refreshed.

## Evidence Service

Authoritative stack ends at PR #22.

PR #23:

- head `3991983c397a222871cba70c6232da1a8be5b265`
- formal review `4850754712`
- disposition: **changes required; partial only**
- confirmed failures: active spec falsely claims Catalog ownership, unsafe/fractional/out-of-range clock values, permissive Date parsing, public stale-threshold override, duplicate wrapper clocks, malformed daemon shape hidden as missing, incomplete preflight routing, no Catalog/CLI/Manager/Node 24.15 evidence
- audit: `docs/audits/evidence-pr23-daemon-liveness-hostile-review-2026-08-03.md`

Queue issue #2 now contains the exact correction criteria. Issue #21 remains blocked on accepted Engine #62 and requires one absolute 2000 ms deadline from earliest foreground visibility through verified complete child-tree termination and lease release.

## Book Forge

PRs #8/#11/#10 remain blocked.

- PR #11 review `4848083104`: path/config/credential authority remains unsafe; queued issue #12 addresses only exact credential-object use.
- PR #10 review `4848065237`: lock initialization/release is not transactional and cleanup can replace primary failures.
- Issue #12 queue branch remains `agent/exact-acquisition-credential-use` at expected base SHA `f698d49b6f9b7bdf8105dde3c598b053502e08f0`.
- Issues #13/#14 still require recoverable snapshot publication and exact catalog/64-shard SQLite authority.

## Lichess Bot

Broad PR #3 remains blocked by hostile review `4848125095`; its 148-test aggregate is not acceptance evidence.

Continue in ownership order:

1. issue #4 exact credential/file/publication/launch identity;
2. issue #5 raw-byte UCI, typed standard options, and immutable applied identity;
3. issue #6 transactional slot activation, failure causality, correlation, and off-loop latest-wins status;
4. issue #7 exact SQLite foundation.

## Immediate continuation order

1. Poll Engine PR #63 and review only a new head beyond `cc2ead92…`; continue only if the session stops without substantive output.
2. Keep the Jules registry synchronized and do not launch Manager queue #3 while `blocked-base-review` remains.
3. Continue hostile review of unchanged broad/provider branches only where no authoritative review already exists; do not duplicate existing reviews.
4. Reconstruct Installer ancestry in order: current PR #2 → corrected PR #3/#4 → replay locally acceptable PR #5 → corrected PR #11.
5. Reconstruct Manager ancestry in order: PR #78 → PR #79 → PR #80 → refresh verified-launch branch/task.
6. When Engine #62 is terminally accepted/rejected and issue #1 closes, launch Evidence queue #2.
7. Continue Evidence #21, Manager #67/#69, Lichess #4–#7, Book Forge #12–#14, and Installer #7–#10.
8. Update both handoffs, registry, and Installer PR #2 discoverability comment before stopping.

## Final gates

No soundness decision until all governing specs/preflight/boundary/substitution checks, authoritative branch reconstructions, native Windows/Linux builds, Node 24.15 provider suites, real ONNX/TT/storage/process/race/recovery/performance evidence, immutable artifact/provenance closure, and repeated clean install/use/repair/upgrade/reinstall/recovery/uninstall/purge lifecycle tests pass.
