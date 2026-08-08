# Engine PR #52 hostile review — canonical FEN admission

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #52 `agent/canonical-fen-admission-v2`  
Reviewed head: `5d122ac47ca80606add8f8354adabb67dbc4a2ff`  
Base: Engine PR #47 / `agent/integrate-engine-audit-stacks` at `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`  
Formal review: `4850845427`  
Disposition: **changes required; keep draft**

## Review result

The early-gate/single-existing-parser architecture is retained. The rejected macro alias and duplicate parser mechanisms did not return. The branch still has canonical-text and ownership defects.

## Confirmed blockers

1. Adjacent empty-run digits such as `44` and `11111111` are accepted because board validation only sums file widths. They normalize to a different output FEN and create multiple textual identities for the same board.
2. The complete inline validator lives in `engine/include/uci_arena/core/fen_validation.hpp`, visible to every Engine translation unit. The architecture guard checks only search directories and does not prove that `Position::from_fen` is the sole production caller.
3. `Position::from_fen` accepts arbitrarily large text/whitespace and then copies it into string/stream state; no domain-appropriate FEN byte bound exists.
4. Canonical numeric text is not resolved: counters with redundant leading zeros are accepted and normalized.
5. Castling-right order is not resolved: arbitrary subset permutations are accepted while `fen()` emits canonical order.
6. Tracked probes omit adjacent digits, exact byte bounds, canonical counter/castling policies, global sole-caller enforcement, and full native repository tests.

## Required correction

- reject adjacent empty-run digits;
- make the validator implementation private to chess core or enforce exactly one production call across all source roots;
- define and test an exact FEN byte bound;
- explicitly choose strict canonical counter and castling text or documented normalization;
- strengthen tracked probes and run full Release Engine/CTest validation.

## Scope guard

Do not expand this structural FEN contract into an ad hoc partial retrograde/history legality validator. Pawn-rank, king-check/adjacency, material reachability, and legal-game-history constraints require a separate explicit chess-core contract if desired.

## Audit consequence

PR #52 is not yet acceptance evidence and Engine issue #48 remains open. The core architecture can be repaired without redesigning the existing Position parser.