# Engine PR #63 fifth hostile review — typed resource-lease revocation

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #63 `jules-13034909149691235140-b7c89938`  
Reviewed head: `2106482635969e1e9c6b28b76fb80aca35890af6`  
Base: blocked Engine PR #47 at `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`  
Review: `4850967412`  
Disposition: **changes required; replayable package slice only; keep draft**

## Review result

Jules performed a real code split into packet, store, error, and facade modules. The formal acceptance criteria remain unsatisfied, and the refactor introduces additional public-state/cache/waiter defects.

## Confirmed blockers

1. Request packets still persist text-only `requested_at` sampled before publication; safe-integer visibility authority is absent.
2. Malformed/missing/future/ambiguous foreground time is still rewritten to observation time and emitted as trusted `foreground-demand`; tests require this invented deadline.
3. Ad hoc observation `contract_version` exists without schema/validator/package-version/consumer-lock migration; package remains 1.0.2.
4. `safeUnlink` renames before identity proof, can overwrite a replacement on restore, and can delete an unverified temporary object on error.
5. Holder heartbeat and request heartbeat remain check-then-path-replace races capable of overwriting replacement authority.
6. Windows and ancestor reparse-safe object identity remain unimplemented.
7. `REQUEST_CACHE`, cache clearing, instrumentation, test-seam symbol, and waiter internals remain exported/public mutable authority.
8. Cache validity uses mutable packet objects and size/mtime without ctime/content identity; global roots interfere.
9. Directory entry limit counts irrelevant/temp debris; invalid reads are not charged to total bytes; each miss allocates the maximum buffer; allocation metrics are manual counters.
10. Equal-time ordering still uses locale-dependent `localeCompare`.
11. Snapshot, demand, acquisition, inherited validation, observation, wait, and compatibility surfaces retain inconsistent failure semantics and producer-shaped legacy markers.
12. Wait construction assigns resolve/reject after watcher/timer setup, permitting synchronous callback failure before continuations exist; cleanup is not exception-complete and debug console output remains.
13. Date/timing domains and producer scalar/format grammar remain incompletely bounded.
14. Tests preserve implementation assumptions and omit replacement races, cache attacks, Windows parity, package migration, exact heartbeat change, and real allocation evidence.
15. PR body remains stale and materially overclaims completion.

## Required direction

- exact versioned packet/result algebra with persisted UTC and safe-integer visibility time;
- platform-specific exact-object store owning read/update/delete;
- private bounded immutable event index/cache;
- private notifier dependency injection and exception-complete transactional wait setup/cleanup;
- one typed failure authority across all public surfaces;
- package-version and consumer-lock migration;
- adversarial Windows/Linux Node 24.15 evidence.

## Ancestry consequence

PRs #39, #44, and #47 are separately blocked. Even a corrected PR #63 is a replayable package-owned slice, not merge-ready Engine ancestry.