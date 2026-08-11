# Engine PR #63 seventh hostile review — resource-lease exact-object authority

Date: 2026-08-04 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #63 `jules-13034909149691235140-b7c89938`  
Reviewed head: `ebf510670da82a22ca55626c8e520fd1af494d6f`  
Base: blocked Engine PR #47 at `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`  
Formal review: `4852208692`  
Disposition: **changes required; rejected; keep draft; replayable package-owned slice only**

## Review result

Jules added a package bump to 1.0.3, private instance fields, a persisted `requested_at_ms`, descriptor-bounded reads, non-locale ordering, and more tests. These are useful partial moves. Its commit and PR claims that the secure redesign and hostile-review corrections are complete are materially false.

## Confirmed blockers

1. `safeUnlink()` verifies an opened descriptor, closes it, then unlinks the mutable pathname. A replacement in that interval is deleted. A narrow local Node 22 probe reproduced `identityMatched=true` followed by `replacementDeleted=true`.
2. Request heartbeat update remains path-based check/open/truncate/write. It can truncate a substituted object, ignores partial `writeSync`, and then adopts replacement `dev`/`ino` authority.
3. Holder creation is closed and later reopened by pathname in the lease constructor, leaving a replacement gap between publication and retained authority.
4. In-place holder truncate/write permits concurrent readers to see malformed transient bytes; `removeStaleFile()` immediately deletes a malformed holder, so an active holder can delete itself.
5. Release closes its retained descriptor before deletion, swallows close failure, uses the same unsafe unlink, and then performs a racy `existsSync` check.
6. Release failure can replace the primary `work()` failure; acquisition cleanup and success-path wait cleanup failures are swallowed; constructor failure can strand holder authority.
7. `requested_at_ms` is neither required nor validated and is ignored during selection. ISO/integer equality, safe-integer authority, old-packet migration, and contradictory-pair rejection are absent.
8. A 1000 ms future skew remains accepted and clamped to observation time, recreating an invented visibility deadline. Tests cover only far-future values.
9. A stale foreground packet whose PID remains live becomes permanent `untrusted-foreground` demand. The test named as covering stale timestamps contains no stale case.
10. Package version 1.0.3 is not a complete migration: packet schema remains v1, observation contract remains 1.0.0, new reasons/failures are unversioned, Evidence pins 1.0.2, Book Forge pins 1.0.0, and no current/previous interoperability or consumer-lock evidence exists.
11. `TEST_SEAMS_SYMBOL` remains publicly exported, `released` is publicly writable, and snapshots expose holder/request `filename` values that reveal private lease paths.
12. Synchronous terminal callbacks can run cleanup before `_watch`/`_setInterval` return; returned handles are assigned after permanent termination and leak. Jules's test invokes a nonterminal callback and misses the race. A narrow local probe reproduced assigned handles with zero close/clear calls.
13. Every waiter owns a watcher and timer with no count/lifetime bound. Successful cleanup failures disappear; raw mutable Error objects are attached on rejection.
14. `realpathSync`/`lstatSync` is pathname check-then-use, not retained Windows ancestor/file handle authority. The test covers an ordinary symlink and silently passes if Windows cannot create it; no junction/reparse-tag/ancestor-replacement proof exists.
15. The 1 MiB byte counter charges only successfully parsed files; up to 200 invalid 64 KiB files can drive roughly 12.8 MiB of reads while `bytes_read` omits them. Allocation metrics are manual counters, irrelevant entries consume the scan budget, and all traversal errors become overflow.
16. `observe()` admits safe integers outside Date's representable range and then throws in `toISOString()`. Publication, heartbeat, observation, cleanup, and ISO generation still sample separate clocks.
17. Failure surfaces remain inconsistent across null, boolean, strings, Error, TypeError, ResourceLeaseError, and conservative observations.
18. The active spec overclaims race prevention and exact wait cleanup. Tests omit the decisive unlink interval, transient-holder read, stale-live-PID behavior, contradictory timestamp pairs, Date range, waiter bounds, wrapper causality, consumer migration, and Windows/Linux Node 24.15 evidence. PR body remains stale and the head has no CI status evidence.

## Independent probe scope

Two isolated Node 22 probes were used only to falsify source claims:

- descriptor verified, closed, pathname replaced, then unlink deleted the replacement;
- terminal callback fired synchronously before notifier constructor return, leaving assigned watcher/timer handles after cleanup had permanently terminated.

These probes do not establish repository-native Node 24.15, Windows, Linux, package, or release readiness.

## Required correction direction

- platform-owned exact-object store ports with retained operation-relative identity;
- complete durable packet publication without transient malformed authoritative bytes;
- exact canonical UTC/safe-integer visibility pair admitted at successful publication;
- typed primary/cleanup failure algebra and verified release result;
- private shared bounded notifier with transactional registration;
- real packet/result/failure versioning plus every consumer lock migration;
- decisive Windows/Linux Node 24.15 race tests.

## Ancestry consequence

PRs #39, #44, and #47 remain independently blocked. Even a corrected PR #63 is replay evidence only until transferred onto reconstructed authoritative Engine ancestry. This head is not merge-ready and is not release evidence.