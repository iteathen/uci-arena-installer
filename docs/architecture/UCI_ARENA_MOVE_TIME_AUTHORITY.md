# UCI Arena Move-Time Authority

Status: normative suite-boundary companion  
Authority: cross-repository ownership and dependency direction  
Composition owner: `uci-arena-installer`

This document defines **who owns which part of move time** across the UCI Arena suite. It does not define the chess allocation algorithm itself. That algorithm remains an Engine/runtime concern governed by its versioned timing-policy contract.

## Canonical model

Move time has several independent pressure sources. They must not be collapsed into one owner merely because they all use milliseconds.

```text
completed-game populations
        |
        v
Timing Evidence Service
  - temporal downstream value
  - minimum useful timing blocks
  - experimentally qualified allocation strategy
        |
        v
immutable timing-policy artifact
        |
        +-----------------------------+
                                      |
current game clock ------------------> Engine move-time owner
increment / moves-to-go -------------->  - current-game feasibility/cost
explicit UCI limits ------------------>  - useful-block authority
transport/output reserve ------------->  - qualified joint strategy
                                      |
                                      v
                              publication time
                                      |
                                      v
                           late-bound move decision
                                      |
                                      v
                                   bestmove
                                      |
                                      v
                             Lichess transport/API
```

Search itself receives none of these timing-policy pressures as search-attention, depth, topology, queue, batching, or worker commands. It searches continuously according to search-owned value and is sampled when move timing says publication is due.

## Multiple authoritative pressures

Two authorities are particularly important and coexist rather than override one another.

### Current game-time authority

The runtime's current clock facts are authoritative over **what is feasible and what spending time now costs**. Relevant facts can include remaining clock, increment, authoritative moves-to-go, explicit protocol time limits, unavoidable local output reserve, and a bounded transport reserve supplied by an external adapter.

A learned timing policy cannot spend time the current game cannot safely afford. Clock pressure may be nonlinear and may dominate near flag risk.

### Minimum useful timing-block authority

Qualified population evidence is authoritative over the **minimum additional elapsed-search opportunity that has demonstrated useful average benefit** in a declared evidence regime.

If the next useful block is 900 ms and only 500 ms is safely available, the runtime does not spend 900 ms and risk the game, and does not reinterpret 500 ms as an evidence-supported useful block. It publishes because no complete supported block is affordable. Unavoidable processing time is not evidence for a smaller useful block.

### Allocation-strategy authority

How those pressures should be composed is an empirical strategy question. Architecture does not permanently choose a greedy, multiplicative, linear, phase-based, reserve-fraction, or other formula.

Timing Evidence compares candidate strategies on controlled many-game populations, freezes candidates, validates them on independent evidence, and publishes the qualified strategy as part of a versioned timing-policy artifact. The Engine applies that strategy to current-game facts. Policy learning never mutates the active game.

Whenever more timing pressures are introduced, the candidate algorithm must represent every material pressure accurately. Hard bounds stay hard; independent inputs stay distinguishable where collapsing them would lose shape, hide interactions, or double-count correlated effects.

## Repository ownership

### UCI Arena Engine

The Engine owns chess move-time application for UCI Arena play:

- receives current clock/protocol facts;
- consumes the qualified timing-policy artifact;
- preserves current game-time authority;
- preserves minimum-useful-block authority;
- executes the qualified allocation strategy across all declared timing pressures;
- decides timing applicability for the current move;
- requests the late-bound search-derived move when publication is due;
- emits `bestmove` through UCI.

The Engine timing owner is downstream from search. It does **not** control search attention, depth, graph topology, batching, queues, workers, or search ownership.

A generic conforming UCI engine may implement its own timing behavior without UCI Arena timing-policy artifacts; Lichess Bot must remain compatible with that substitution.

### Timing Evidence Service

Evidence Service owns **timing-policy production**, not live clock application:

- population experiment orchestration;
- durable completed-game evidence;
- temporal downstream-value analysis invocation;
- minimum useful timing-block discovery;
- candidate allocation-strategy comparison;
- discovery versus held-out partitions;
- frozen candidate lifecycle;
- qualification and immutable policy publication.

It does not own current game-clock truth, runtime move-time decisions, search behavior, or Lichess transport timing.

### Lichess Bot

Lichess Bot owns **transport/API timing**, not chess-thinking strategy:

- authoritative Lichess stream clock projection to the UCI child;
- local state-processing measurement;
- provider transport reserve;
- move-request queueing and priority;
- rate-limit/cooldown behavior;
- current-state revalidation before dispatch;
- HTTP move submission and acknowledgement.

The provider may supply its measured transport reserve through the Engine's declared overhead/safety input. It does not interpret useful blocks or allocation strategy.

After receiving a legal authoritative `bestmove`, the provider adds no chess-thinking delay. It submits as soon as its state/queue/rate-limit/transport policy permits.

### Manager

Manager is a control-plane projector. It may show configured/qualified/missing/rejected timing artifacts and provider transport status from typed owner projections. It does not calculate per-move time, parse policy internals to invent qualification, combine clocks with policy coefficients, or merge Engine timing and Lichess transport timing into one authority.

### Installer

Installer composes, validates, installs, preserves, upgrades, rolls back, and wires declared timing-policy/profile artifacts. It does not interpret their chess semantics, useful-block thresholds, allocation strategy, or live clock state.

### Book Forge

Book Forge produces opening-book artifacts and has **no move-time authority**. Whether a consumed book artifact resolves the current move is an Engine/runtime knowledge-source decision.

## Timing applicability and non-search resolution

Search-time allocation applies only while the final publication move remains search-derived.

A configured non-search authority can bypass search-time allocation only when it actually resolves one legal move for the exact current position under its own policy.

Examples:

- opening policy commits one move -> timing not applicable;
- opening book supplies only a hint/prior/seed -> timing remains applicable;
- exact tablebase policy resolves the current move -> timing not applicable;
- tablebase supplies only WDL safety/candidate constraints/internal proof evidence -> timing remains applicable.

This is an Engine/runtime applicability decision. Evidence Service may recommend the classification semantics; Lichess Bot and Manager must not infer it from provider availability.

A resolved non-search move proceeds toward emission immediately, subject only to protocol and external transport/API policy. It must not be represented as a `0 ms` searched treatment sample.

## Evidence interpretation

Whole-game evidence may retain every move because book/tablebase moves still affect downstream causal outcomes. Timing-policy analysis must nevertheless distinguish:

- search-derived timing-applicable moves;
- non-search-resolved timing-not-applicable moves;
- useful blocks purchased;
- useful blocks skipped or unaffordable;
- emergency publication below the next useful block;
- current clock/increment/reserve state;
- search-progress observations;
- decision latency;
- transport/API latency.

Emergency publication below a useful block does not prove that a smaller useful block exists.

## Compatibility evolution

Existing timing-profile and temporal-curve schemas keep their historical meaning. Introducing minimum-useful-block authority, allocation-strategy identity, or explicit timing-applicability outcomes requires versioned contract evolution. No repository may silently reinterpret an old artifact to mean the new contract.

Installer and Manager treat new artifacts through declared component/service contracts. Lichess Bot adds extended-adapter validation only when the corresponding Engine contract version exists. Standard UCI compatibility remains independent.

## Acceptance across the suite

A conforming implementation proves:

1. changing only current clock can change move-time allocation without changing search attention or search policy;
2. changing only the qualified minimum useful block can change whether another search-time purchase is available without changing the clock facts;
3. no complete useful block affordable results in publication rather than unsafe overspend or an invented partial useful block;
4. multiple allocation strategies are substitutable through one policy contract and are selected by evidence, not hard-coded suite composition;
5. book/tablebase resolution bypasses timing only when one move is actually resolved;
6. Lichess Bot adds no chess-thinking delay after `bestmove` and keeps transport timing distinct;
7. Manager projects but does not interpret timing semantics;
8. Installer composes but does not interpret timing semantics;
9. Evidence Service learns policy but does not apply live clock decisions;
10. search remains timing-policy blind and continuous.
