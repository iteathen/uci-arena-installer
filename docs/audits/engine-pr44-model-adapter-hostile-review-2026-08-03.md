# Engine PR #44 hostile review — provider-neutral model adapter

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #44 `agent/model-adapter-contract`  
Reviewed head: `d37ab900486cad25b5dff5cd6fa11724c8fc3929`  
Base: Engine PR #39 / `agent/system-sanity` at `1ea05b1e6481e6eba168138d6fac2d73bf0ce4ee`  
Formal review: `4850859460`  
Disposition: **changes required; foundation redesign; keep draft**

## Review result

The PR correctly removes the concrete ONNX evaluator dependency from the model-semantic header. The new universal interface still encodes the current ONNX adapter’s accidental runtime mechanics and is not a sound provider-neutral foundation.

## Confirmed blockers

1. Fixed `kPreparationSlotCount = 2` and numeric slot parameters embed current concurrency policy in model semantics.
2. Dynamic metadata rank conflicts with a four-axis tensor view; supported dtypes are restricted to int32/float32 without a justified domain contract.
3. Mutable `void*` crosses the boundary without byte extent, alignment, lifetime, aliasing, host/device memory, or synchronization rules.
4. Immutable model semantics, mutable batch allocation, row materialization, and runtime buffer publication are conflated in one stateful adapter.
5. Raw filesystem paths are exposed as semantic contract artifacts without immutable identity, digest, role, root authority, or lifetime rules.
6. Policy index uses an untyped int/sentinel and value semantics are reduced to an unstated scalar white multiplier.
7. Batch/dimension/byte inputs are unbounded and have no transactional failure algebra.
8. No dedicated registered C++ ModelAdapter contract defines invariants, memory ownership, compatibility, or lifecycle; the LEGO guard proves only forbidden words are absent.
9. The fake substitution repeats the same two-slot host-float-vector/fixed-rank design and proves inheritance rather than materially different substitution.
10. ONNX compatibility aliases have no bounded deprecation/removal plan and can keep generic code coupled to legacy names.

## Required architectural direction

- immutable model-semantic contract for identity, feature schema, policy mapping, value perspective, and repetition semantics;
- separate project-owned feature materializer writing to an explicit caller/workspace destination;
- inference backend/session owns batching, staging memory, concurrency, synchronization, and execution;
- rank/type/layout/byte descriptors share one domain-appropriate algebra;
- immutable artifact references rather than raw mutable paths;
- typed bounded policy/value/error results;
- genuinely diverse substitution tests and a registered governing specification.

## Audit consequence

PR #44 is not acceptance evidence. Engine PR #47 and every descendant, including active Jules PR #63 and PR #52 ancestry, must be reconstructed or rebased only after the model-adapter foundation is corrected. The dependency-direction improvement should be preserved.