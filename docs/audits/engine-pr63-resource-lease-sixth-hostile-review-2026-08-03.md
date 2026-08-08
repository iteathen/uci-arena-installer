# Engine PR #63 sixth hostile review — typed resource-lease revocation

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #63 `jules-13034909149691235140-b7c89938`  
Reviewed head: `e7eb5edfbb584aff55e2414b36d769e0b4405fc0`  
Previous substantive head: `2106482635969e1e9c6b28b76fb80aca35890af6`  
Base: blocked Engine PR #47 at `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`  
Formal review: `4851511234`  
Disposition: **changes required; replayable package-owned slice only; keep draft**

## Review result

The completed Jules correction is substantive. It makes the cache and waiter set private, removes the prior rename/restore deletion implementation, adds fatal UTF-8 and descriptor checks to bounded reads, and assigns promise continuations before notifier construction. These are useful local improvements, but the issue contract and fifth-review acceptance criteria remain unsatisfied.

## Confirmed blockers

1. Request publication still persists only textual `requested_at`; no canonical safe-integer Unix-millisecond visibility authority is stored.
2. Malformed, missing, future, or ambiguous request time is rewritten to filesystem birth/ctime and emitted as trusted `foreground-demand`. The focused test requires this invented deadline.
3. Future heartbeats inside a one-second tolerance remain live, allowing negative elapsed time and later staleness authority.
4. Observation `contract_version` remains ad hoc; no result schema/validator or consumer migration exists, and the package remains `1.0.2`.
5. The public reason algebra adds `directory-overflow` and `untrusted-foreground` beyond the required bounded reason set.
6. Holder/request heartbeat replacement remains check-then-open/truncate by mutable pathname and can overwrite a substituted object.
7. `safeUnlink()` verifies an opened dev/inode, closes it, then unlinks by pathname; a replacement between close and unlink is deleted.
8. Request cleanup identity is refreshed after path-based heartbeat replacement, so a substituted object can become deletion authority.
9. `verifyReparseAuthority()` is pathname observation only; ancestor identities are not held and Windows junction/reparse guarantees are absent.
10. Acquisition, release, heartbeat notification, abort-listener removal, watcher/timer cleanup, and holder deletion continue to swallow cleanup failures.
11. `release()` reports released even when exact holder removal failed and the disk holder remains authoritative.
12. Wait registration still leaks when `_setInterval` or `_watch` calls back synchronously before returning its handle. A narrow local Node 22 probe resolved the wait while retaining one active timer. This is falsification evidence only, not supported-runtime readiness.
13. `staleMs`, acquisition `pollMs`, `timeoutMs`, path/root bytes, cancellation latency, total work, and free-space policy are not exactly admitted.
14. `observe()` accepts safe integers outside JavaScript `Date` range and can throw while constructing a supposedly typed observation.
15. Directory entry limits count unrelated debris; invalid-file bytes are not charged; cache-hit reads disappear from byte metrics; allocation metrics are manual counters.
16. Equal-time ordering remains locale-sensitive through `localeCompare`.
17. Snapshot and inherited-validation surfaces expose the lease root and per-request filenames; diagnostics can expose absolute paths.
18. Public failure behavior still mixes null, booleans, plain errors, typed errors, free-form strings, snapshots, and synthetic observations.
19. Tests encode invalid timestamp rewriting and manual instrumentation rather than attacking exact publication/update/delete races.
20. Windows/Linux Node 24.15 native behavior, package migration, locale variance, release-cleanup failure, synchronous callback-before-return, ancestor substitution, and exact-byte packet/result tests remain absent.
21. The PR body still claims complete success and all criteria met despite blocked ancestry and repeated hostile review failures.

## Required direction

- persist canonical UTC and safe-integer millisecond visibility fields in the packet at successful exact-object publication;
- give one platform-specific store exact create/read/update/delete ownership using held parent/object identity and Windows reparse-safe native support;
- introduce one versioned packet/result/failure algebra and migrate the package plus every consumer lock;
- make notifier injection private and registration/cleanup transactional under synchronous callbacks and throwing cleanup;
- replace manual metrics with enforceable byte, entry, time, allocation, cancellation, and free-space bounds;
- add adversarial Node 24.15 Windows/Linux tests for final/ancestor replacement, junctions, exact deletion/update identity, cleanup causality, notifier races, ordering, and package migration.

## Ancestry consequence

PRs #39, #44, and #47 remain independently blocked. PR #63 remains a replayable package-owned correction workspace only and is not merge-ready Engine ancestry. `PKG-E2E-011` remains unknown; no soundness or release-readiness conclusion exists.
