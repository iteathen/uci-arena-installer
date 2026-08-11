# Engine PR #39 hostile review — evaluation and release foundations

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #39 `agent/system-sanity`  
Reviewed head: `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`  
Base: `main` at `fa4abec84fffdc0cd3942004db11179fd3e7413a`  
Formal review: `4850912840`  
Disposition: **changes required; foundation reconstruction; split required**

## Review result

The PR contains reusable sub-slices—especially removal of the concrete ONNX include from search and rejection of OS-owned Windows UCRT/API-set files—but it cannot serve as one authoritative foundation. It mixes 36 files and unrelated ownership boundaries while introducing public evaluation and release-evidence defects.

## Critical evaluation blockers

1. `OrderingScoreBatch::reset(count)` writes `count` elements into fixed arrays without proving `count <= kMaxMoveListSize`, permitting memory corruption.
2. Completed-cache capacity returns a minimum of 64 entries even when one policy vector exceeds the entire 64 MiB budget, allowing allocations orders of magnitude above the declared cap.
3. Async request/result/lease contracts retain unowned raw pointers to positions, move lists, evaluators, callback payloads, and service internals.
4. Arbitrary non-noexcept consumer callbacks execute under the completed-cache mutex, enabling deadlock, blocking, reentry, or worker failure.
5. `ReadHandle` transfers ownership of the global cache mutex to callers for arbitrary duration, adding hidden hot-path serialization.
6. Ticket, batch, and cache capacities are hardcoded into the public class rather than validated composition/runtime policy.
7. The generic evaluator interface carries current neural/model/cache/debug assumptions rather than minimal provider-neutral capabilities.
8. Capability defaults can fabricate child-policy/value support instead of requiring explicit declaration.
9. No registered evaluation-service specification defines ownership, lifetimes, cancellation, thread safety, errors, or compatibility.

## Critical release/toolchain blockers

1. Two “independent” build roots are compared only by path string and can alias the same physical directory.
2. Build roots are not bound to the receipt’s source commit/tree; stale or unrelated CMake trees can be paired with current HEAD.
3. Compiler/linker/generator identities can validate without executable digests.
4. Receipt files and nested fields are byte-unbounded and accept extra structure before entering signed results.
5. Tool executables are path-stat-then-full-read hashed, allowing replacement races and unbounded memory use.
6. Post-build hashing does not prove the exact executable bytes actually performed compilation/linking.

## Scope/version blockers

- Evaluation architecture, search/UCI migration, native-helper packaging, release discovery, toolchain attestation, workflow, CMake, and specs are mixed in one PR.
- Package actually changes 1.0.0 → 1.0.2 while PR prose says 1.0.1; later API changes reuse 1.0.2.
- Reported packaging tests do not establish evaluation concurrency/lifetime safety or release-receipt integrity.

## Required reconstruction

Split into independently reviewable ancestry:

1. minimal evaluation semantics/service contract with bounded ownership-safe async/cache behavior;
2. search/UCI migration and compatibility removal;
3. native-helper OS-runtime packaging;
4. exact native toolchain/source/build receipt;
5. build/test topology.

## Audit consequence

PR #39 is not authoritative Engine ancestry. PR #44, PR #47, PR #52, and active Jules PR #63 are all parent-blocked and must be replayed only onto corrected foundational slices.