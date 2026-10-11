# UCI Arena move-time authority

Status: suite ownership boundary, reconciled from historical
[PR160](https://github.com/iteathen/uci-arena-installer/pull/160).
Current consumer and provider contracts govern their implemented capabilities;
this document introduces no runtime option or policy schema.

The current suite engine product is independent **Vector**, implemented in
JavaScript/restricted Device-JS over public cuda-js/Tensor/MCGS contracts. The
cancelled C++ chess/search product is not a deployment fallback. Generic UCI,
search semantics and independently published native SDK/referee/process helpers
retain their owning roles.

| Owner | Authority |
| --- | --- |
| Actual game consumer | Original/remaining clock facts, increment and declared protocol limits |
| Engine/search completed-result owner | Maintain the occupied completed bestmove memory slot; publish updates continuously as progress completes |
| Vector or conforming move-time runtime | Apply a qualified policy to current game facts; decide applicability and the downstream memory-read/delivery deadline |
| Evidence Service | Durable experiments, population identity, frozen candidate and qualification/publication lifecycle through declared external runtime/analysis capabilities |
| Declared analysis runtime | Timing/statistical algorithm and search-observation semantics |
| Lichess Bot | Stream-clock forwarding, measured transport/API reserve, queue/rate-limit/submission timing |
| Manager | Exact consumer clock forwarding, typed owner operations and bounded readiness/status projection |
| Installer | Opaque artifact identity, compatibility, composition and installation |
| Book Forge | Opening-book production; no move-time allocation authority |

## Clock facts and qualified policy

Actual game clocks govern feasibility and opportunity cost. A policy cannot
override remaining time or infer the original control from an evidence profile,
placeholder option or diagnostic duration. A declared useful block describes an
evidence-supported additional elapsed-search opportunity in a particular regime;
an unaffordable block does not establish a smaller supported one. Internal search
batches and useful timing blocks are different concepts.

The allocation strategy itself requires experimental comparison and independent
qualification. Architecture specifies no universal greedy formula, complexity
multiplier, centipawn threshold or fixed duration table. When material, clock,
increment, hard reserves, useful blocks, temporal value, instability, ambiguity
and stable decisive-candidate evidence retain their different meanings. A large
unstable candidate lead is not automatically an easy move.

Search owns attention, topology, work, queues and move selection. The engine/search
owner maintains its occupied completed bestmove memory slot and publishes updates
continuously as progress completes. The downstream timing owner may observe
declared bounded search evidence and select a bounded memory-read/delivery
deadline; it does not schedule or defer search's completed-slot publication and
sends no policy/clock control back into search.

At the deadline, the consumer reads the already-completed compatible current-focus
slot. If a newer update is in flight, it uses the available older completed slot.
No search wait, new evaluation, GPU readback, profile/policy reload or catch-up
publication is required at the deadline. Search may continue progressing and
publishing completed updates independently. Profile/policy refresh remains
between games. Population learning uses retained completed-game observations,
rather than learning or rewriting policy during an active game.

## Exact consumer-clock forwarding

The user accepted SP-004: Manager may forward original/remaining UCI clocks and
increment unchanged while timing calculations remain with their existing owners.
This preserves reviewed Manager 0.3.29 behavior, not a second timing algorithm.
Its explicit probe control and a competition's original TimeControl bind only
producer-declared clock options before readiness. Unrelated managed profile
arguments/options and policy identity remain exact.

Evidence runtime-v2 likewise admits exact inventory-bound targets and explicit
campaign original controls through its public contract. A diagnostic `timeMs`
budget is not an original `initialTimeMs`/`incrementMs`. Baseline zero placeholders
grant no actual-clock authority. Each fresh/resumed game admits its profile and
original control at its consumer-owned boundary; profile/policy/hash/Require flags
are not rewritten during an active game. Missing/incompatible admission fails
under the owner contract without silently disabling a required policy.

After a legal authoritative `bestmove`, Bot adds no chess-thinking delay. Its
transport reserve, queue, revalidation, cooldown and API timing remain separate
from engine timing-policy qualification. Manager projects these distinct verdicts
and invokes declared owner operations; it does not replay policy coefficients,
population evidence or statistical math. Installer neither interprets those
semantics nor invents unavailable provider operations.

## Evidence and applicability limits

Population discovery, frozen candidates and independent held-out qualification
are separate stages. One game/move/position cannot establish useful-block,
local-pressure or strategy authority. Current Evidence scheduling/qualification
is a subset of this production direction; this document claims no completed
population pipeline or expanded current policy support.

Only actual resolution of one legal move for the exact position by a declared
book/tablebase policy can bypass search-time allocation. Hints, priors, WDL
constraints and provider availability do not establish resolution. Such moves
may remain whole-game causal evidence but are not zero-time searched treatments;
deadline read/delivery below a useful block does not prove a smaller block.

Existing timing-profile, temporal-curve, runtime-v2 and clock-binding contracts
retain their exact meanings. New useful-block/strategy output schemas require
explicit versioned owner contracts. Current Vector qualification remains within
its documented runtime/hardware/control and campaign scope: installed readiness,
targeted regressions and mixed discovery campaigns are not general strength or
universal timing-law evidence.
