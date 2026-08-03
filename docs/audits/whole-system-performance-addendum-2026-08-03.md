# UCI Arena whole-system performance addendum — 2026-08-03

## Scope and authority

This addendum extends the whole-system sanity ledger with mandatory hot-path
criteria discovered during the line-by-line Engine audit. It does not transfer
Engine implementation ownership to Installer. Installer owns this suite-level
acceptance record; each implementation repair remains in its owning product
repository and enters the suite only through immutable release artifacts.

## Mandatory hot-path rules

The following are release-blocking design constraints for Engine search,
canonical position execution, model-input preparation, inference scheduling,
and shared caches:

1. No text, JSON, schema, FEN, UCI, or provider-format transformation may occur
   per node, per edge, per legal move, or per evaluation request after the
   owning boundary has admitted the data.
2. No heap allocation, unbounded growth, filesystem work, subprocess work, or
   blocking I/O may occur in node search, move generation, make/unmake, attack
   detection, transposition lookup, or evaluator hot submission paths.
3. Parallel search must distribute real move-search or graph-search work. A
   background thread that does not contribute useful searched positions is not
   evidence of scaling.
4. Per-node synchronization must be lock-free or demonstrably bounded and
   contention-tolerant. Contention in an optional cache should normally degrade
   to a cache miss rather than block useful search.
5. CPU-feature choice is resolved outside repeated hot loops. Per-node feature
   dispatch, runtime-library branches, and generic wrappers that weaken known
   intrinsic code generation are forbidden unless measured faster end to end.
6. Existing `_BitScanForward64`, `_BitScanReverse64`, compiler `ctz/clz`,
   prefetch intrinsics, CPUID intrinsics, and `xgetbv` assembly paths are
   preserved unless generated-code and end-to-end evidence proves a stronger
   replacement.
7. BMI2, AVX2, AVX-512, VNNI, PEXT/PDEP, and handwritten assembly are admitted
   only behind architecture-appropriate composition or once-per-session
   dispatch with correctness parity and representative CPU measurements.
8. Performance acceptance is useful searched work and playing-strength-relevant
   latency under representative thread counts. A faster isolated instruction,
   local counter, or raw node rate is insufficient when total search quality or
   parallel scaling regresses.

## Resolved canonical-state findings

### ENGINE-CORE-001 — malformed FEN admission

**Status:** repair proposed, pending review and merge.

**Owner:** Engine chess core.

**Evidence:** Engine draft PR `iteathen/UCI_Arena#42` places allocation-free
strict validation at `Position::from_fen`, keeps the existing state-construction
primitive private, and adds a standalone compiler probe. It rejects malformed
field counts, rank geometry, numeric counters, castling rights, and en-passant
state without adding work to search or make/unmake.

### ENGINE-CORE-002 — phantom-rook castling

**Status:** repair proposed, pending review and merge.

**Owner:** Engine chess core.

**Evidence:** the same Engine PR requires physical king/rook origin state for
every admitted castling right. Existing incremental move execution already
removes rights when the king or rook moves or the rook is captured, so the
invariant is established once at admission rather than rechecked per node.

## Critical open performance finding

### ENGINE-PERF-041 — transposition-table shard mutex on every probe and store

**Severity:** critical for multicore search scaling.

**Status:** confirmed; dedicated Engine repair required.

**Observed implementation:**

- 1,048,576 transposition clusters;
- three entries per cluster;
- 4,096 shared `std::mutex` shards;
- every probe acquires a `TimedTranspositionLock` before scanning its cluster;
- every store acquires the same shard lock before update or replacement; and
- 256 independent clusters therefore contend on each operating-system mutex.

**Why this is unsound for the hot path:** transposition access is a per-node
operation. Blocking a search worker on an unrelated cluster converts cache
coordination into a global throughput tax and directly opposes the suite's
real-parallel-search requirement.

**Required replacement properties:**

1. probes do not take an operating-system mutex;
2. a reader either obtains one stable published entry or treats contention as a
   miss; it never consumes torn key/payload state;
3. writers preserve exact/depth/bound/generation/replacement semantics;
4. publication uses lock-free 64-bit atomics on supported targets, proven by a
   compile-time gate;
5. writer contention is bounded at the exact cluster, not at a 256-cluster
   shard, and may reject a cache write rather than delay search;
6. reset/invalidation occurs only under the existing quiescent search-state
   authority or uses an equally safe generation transition;
7. existing transposition correctness, cancellation, and exact-search probes
   pass unchanged;
8. a concurrent torn-read stress probe is added; and
9. acceptance requires both single-thread non-regression and useful multicore
   throughput improvement at representative thread counts.

**Non-solution:** changing the number of mutex shards, replacing `std::mutex`
with a spin lock without bounded contention behavior, or enabling a CPU
instruction without end-to-end proof does not close this finding.

## Additional parallelism review queue

These are review targets, not yet defects:

- MCGS graph ownership currently uses a graph-wide mutex around selection and
  several backup/publication paths; its lock hold time and worker scaling must
  be measured before changing ownership.
- Killer-move tables use per-ply mutexes while quiet-history cells are atomic;
  frequency and contention must be measured after TT synchronization is removed.
- Root scratch vectors resize at root-search boundaries rather than per node;
  this is acceptable unless profiling shows repeated capacity churn.
- Native PEXT/PDEP is detected by CPU diagnostics but remains deliberately
  disabled. It is a benchmark candidate, not an assumed improvement.

## Suite acceptance impact

The final clean packaged-suite cycle remains required. In addition, an Engine
artifact containing the TT mutex design above cannot be considered
performance-qualified for multicore release until `ENGINE-PERF-041` is closed
or representative measurements prove the current synchronization cost is below
the declared threshold. The latter is considered unlikely but must be decided
by evidence, not assumption.
