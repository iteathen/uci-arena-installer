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
9. Public architecture boundaries are coarse. A model, inference, knowledge, or
   search implementation may use static or devirtualized inner loops, but it
   must not pay one virtual dispatch per node, move, tensor row, or cache cell.
10. Stable immutable data should be shared or viewed, not copied under a global
    mutex. Callback or projection work never executes while holding a service-
    wide lock unless a measured fixed upper bound and reentrancy proof exist.

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

## Critical open performance findings

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

### ENGINE-PERF-042 — MCGS simulations serialize on one graph-wide mutex

**Severity:** critical for persistent MCGS scaling.

**Status:** confirmed; dedicated Engine repair required.

**Observed implementation:** one `graph_mutex_` protects the complete persistent
MCGS graph. The lock is held while:

- selecting a path and updating virtual outstanding counts;
- generating legal moves for unexpanded leaves;
- creating child positions and graph nodes;
- backing up every completed simulation through the full path;
- recomputing proof state; and
- every 32 completions, sorting root edges and rebuilding principal variations
  for publication.

Provider inference can execute concurrently, but all graph selection and backup
work funnels through one critical section. Extra workers therefore cannot scale
graph work independently and may queue behind root sorting/PV construction.

**Required replacement properties:**

1. immutable node identity and append-only graph storage have stable addresses;
2. selection uses atomic edge/node statistics or bounded fine-grained ownership;
3. expansion has one exact owner while unrelated leaves remain selectable;
4. backup updates do not require a graph-global lock;
5. root snapshot construction reads one consistent published view outside the
   simulation critical path;
6. stale generation and reservation semantics remain intact;
7. proof propagation remains deterministic and race-safe; and
8. acceptance measures simulations/second, completed useful depth, reroot
   latency, and playing-strength-relevant output from one through the supported
   worker maximum.

**Non-solution:** replacing the global mutex with a global spin lock or merely
moving inference outside the lock does not close the finding; inference is
already outside most graph mutation sections.

### ENGINE-PERF-043 — completed-evaluation cache exposes one global lock to consumers

**Severity:** high for parallel batching and move ordering.

**Status:** confirmed; provider-neutral Evaluation Service repair required.

**Observed implementation:**

- cache probe, admission, ticket state, and completed results share one mutex;
- `ReadHandle` deliberately keeps that mutex locked while the caller reads an
  `Evaluation`;
- cached completion callbacks execute under the same mutex; and
- real ONNX consumers iterate legal moves, call policy-index mapping for every
  move, copy policy/value projections, and in some paths copy the complete
  `Evaluation` vector while the service-wide lock remains held.

This is a coarse service lock around variable consumer work. One cache hit can
therefore delay unrelated cache probes, ticket admission, batch formation,
completion, and other search workers.

**Required replacement properties:**

1. completed evaluations are immutable after publication;
2. a cache hit pins a stable immutable result with a bounded pointer/handle
   operation, then releases service coordination before projection;
3. callbacks execute outside the service mutex and may not invalidate the
   pinned result;
4. ticket/batch coordination remains separately synchronized;
5. no policy vector is copied merely to escape a lock unless measurement proves
   that cheaper than stable immutable ownership; and
6. reentrancy, cancellation, cache eviction, and generation invalidation tests
   accompany multicore cache-hit and mixed hit/miss benchmarks.

### ENGINE-PERF-044 — scalar per-row tensor transformation behind virtual dispatch

**Severity:** high for inference-fed search throughput.

**Status:** confirmed; model-adapter and inference-port redesign required.

**Observed implementation:**

- `OnnxInputAdapter` exposes virtual `materialize_row`;
- the executor invokes that boundary once per evaluation row;
- dense-plane adapters clear 1,088 or 1,216 floats per row;
- they reread all 64 board squares, branch on piece characters, rotate and swap
  representations for black-to-move, then fill repeated scalar planes; and
- model artifacts declare a 68-word canonical hot-loop ABI and binary symbol
  table, but runtime materialization only validates those artifacts and does not
  consume the ABI.

The only unavoidable boundary is conversion into the model's declared tensor.
The current design adds avoidable per-row virtual dispatch, full-board character
scans, and repeated dense clears instead of composing a batch-specialized
adapter over canonical bitboards/state words.

**Required replacement properties:**

1. one coarse virtual or composed call materializes a complete batch;
2. the concrete adapter owns a devirtualized inner loop;
3. canonical piece bitboards/state words enter directly, without FEN, strings,
   piece-case swapping, or intermediate object conversion;
4. constant planes use preinitialized templates, vectorized stores, or provider-
   appropriate device preparation rather than repeated scalar fills;
5. adapter output remains exact against golden vectors and policy mapping;
6. generated-code inspection proves intended SIMD/assembly is emitted;
7. CPU materialization, transfer overlap, provider service, and total search
   throughput are measured separately; and
8. an incremental feature cache is admitted only if make/unmake update cost and
   memory locality outperform batch reconstruction end to end.

### ENGINE-GOV-045 — active model contracts retain legacy project identities

**Severity:** medium governance defect with migration risk.

**Status:** confirmed; model-package migration required.

The LatticeKnight and Rookstorm runtime adapters require active contract schema
strings prefixed with the legacy project identity, including the declared
hot-loop ABI. Renaming source checks alone would make existing model packages
unloadable. The owning model-package contracts, golden vectors, binary symbol
header, generated adapters, validation reports, and Engine checks must move as
one versioned compatibility migration. The legacy identifiers may remain only
as explicit old-schema migration inputs, never as the current UCI Arena schema.

## Additional parallelism review queue

These remain review targets rather than confirmed defects:

- Killer-move tables use per-ply mutexes while quiet-history cells are atomic;
  frequency and contention must be measured after TT synchronization is removed.
- Root scratch vectors resize at root-search boundaries rather than per node;
  this is acceptable unless profiling shows repeated capacity churn.
- Native PEXT/PDEP is detected by CPU diagnostics but remains deliberately
  disabled. It is a benchmark candidate, not an assumed improvement.
- ONNX Runtime is configured with one intra-op and one inter-op thread to avoid
  nested CPU oversubscription. Provider-specific alternatives require whole-
  engine batch and search measurements rather than a library-only benchmark.

## Suite acceptance impact

The final clean packaged-suite cycle remains required. In addition, an Engine
artifact cannot be considered performance-qualified for the supported multicore
configuration while `ENGINE-PERF-041`, `ENGINE-PERF-042`, `ENGINE-PERF-043`, or
`ENGINE-PERF-044` remains open without representative evidence disproving its
material impact. Source-level reasoning strongly indicates each is material,
but final acceptance is based on generated code and end-to-end useful-search
measurements rather than assumption.
