# UCI Arena Complete LEGO Architecture

Status: target architecture  
Authority: suite-level product and repository boundaries  
Composition owner: `uci-arena-installer`

This document defines the intended ownership, dependency direction, substitution boundaries, and release topology for the complete UCI Arena suite. It is a target: current repositories may need staged migrations, but new work must reduce the distance to this structure and must not introduce new cross-boundary coupling.

## 1. Product topology

The suite consists of six independently owned product repositories:

```text
uci-arena-installer
        |
        +-- uci-arena-engine
        +-- uci-arena-manager
        +-- uci-arena-lichess-bot
        +-- uci-arena-book-forge
        `-- uci-arena-evidence-service
```

The installer assembles the suite. Manager is the installed control plane. The engine owns chess and search. Lichess Bot, Book Forge, and Evidence Service are external provider products. Repositories communicate only through published contracts and immutable release artifacts; they never import one another's internal source trees.

## 2. Repository ownership

### 2.1 `uci-arena-engine`

The current `UCI_Arena` repository is the engine brick and should eventually be renamed without changing its product identity or history.

It owns:

- chess position, legal move, make/unmake, repetition, draw, and terminal truth;
- UCI protocol behavior;
- formal search contracts and the minimax and MCGS search implementations;
- evaluation orchestration;
- model-semantic adapters;
- inference-runtime ports and concrete runtime adapters, including ONNX Runtime;
- opening-book and tablebase consumption;
- engine diagnostics and correctness tests;
- engine-specific evidence runtime tools; and
- engine component packaging.

It does not own Lichess API behavior, suite management, product installation, opening-book acquisition, evidence scheduling, account credentials, or suite composition.

### 2.2 `uci-arena-manager`

Manager is one product containing its service, CLI, native desktop host, web dashboard, privileged broker, persistence, provider adapters, and local contracts.

It owns:

- the installed local API and persistent control-plane state;
- product status, component discovery, and provider registration;
- typed lifecycle and process controls;
- configuration and health projection;
- operator authorization;
- account and bot control projection; and
- orchestration of installed providers.

It does not own provider behavior. Provider calls use fixed typed operations, never provider source imports or arbitrary shell commands. Manager may display a Lichess option, but Lichess Bot remains authoritative for its validation, token access, challenge policy, persistent state, and game execution.

### 2.3 `uci-arena-lichess-bot`

Lichess Bot owns all BOT integration:

- API transport, event/game streams, reconnect, recovery, and rate limiting;
- inbound and outbound challenge policy and opponent selection;
- game reconstruction, move submission, and one UCI process per admitted game;
- scheduling, reservations, and capacity admission;
- account probe, persistent account state, and bot telemetry;
- token-file access; and
- bot configuration, status, startup, and packaging.

It consumes a UCI executable as an external process and must work with any conforming engine. It cannot depend on UCI Arena Engine internals.

The token is operator-owned. Only Lichess Bot may read its contents. Manager sees a path and redacted status; Installer never reads, copies, hashes, logs, or packages it.

### 2.4 `uci-arena-book-forge`

Book Forge owns opening-book production: acquisition, Explorer and PGN ingestion, observations, frontier selection, Stockfish qualification, overlap authority, compilation, bounded retention, immutable publication, daemon lifecycle, and health/status. It publishes declared book artifacts. The engine consumes those artifacts without knowing how they were acquired or qualified.

### 2.5 `uci-arena-evidence-service`

Evidence Service owns scheduling, admission, resource isolation, qualification workflows, timing-profile generation, normalization, result catalogs, resumability, datasets, publication, daemon lifecycle, and health/status. It invokes declared runtime tools from the engine artifact. It does not own chess legality or search.

### 2.6 `uci-arena-installer`

Installer is the suite composition and deployment product. It owns:

- component catalogs and product compositions;
- immutable artifact acquisition, signature, provenance, checksum, and compatibility verification;
- environment and existing-state inspection;
- installation planning, staging, validation, deployment, backup, rollback, and receipts;
- preservation, upgrade, repair, uninstall, and result reporting;
- Windows and Linux package assembly; and
- complete-suite installation tests.

It consumes component artifact + manifest + checksums + provenance + compatibility. Provider source or build paths such as `../UCI_Arena/build`, `../uci-arena-manager/dist`, or provider output directories are forbidden inputs.

## 3. Cross-repository component contract

Every installable repository publishes versioned component and, where applicable, service contracts. A release normally contains:

```text
release/
  component artifact
  component-manifest.json
  service-contract.json        # service products only
  configuration-schema.json    # service products only
  status-schema.json           # service products only
  checksums.json
  provenance bundle
  version-and-compatibility.json
```

The component manifest declares identity, version, artifact, entrypoints, working directory, health checks, owned/preserved/generated paths, prerequisites, required components, minimum installer, and Manager API compatibility. Each provider owns its manifest content. Installer owns composition, compatibility resolution, and deployment mechanics.

Example shape:

```yaml
component:
  id: uci_arena.engine
  version: 2.4.0
runtime:
  entrypoint: bin/uci_arena_engine.exe
  working_directory: .
  health_checks:
    - type: uci
      commands: [uci, isready]
installation:
  owned_paths: [bin, models]
  preserved_paths: []
  generated_paths: [runtime]
  required_components: []
compatibility:
  installer_minimum: 1.2.0
  manager_api: 3
```

Contracts use project-owned schemas and types. Third-party SDK objects, private storage records, raw shell command strings, and provider-internal paths cannot cross a repository boundary.

### 3.1 Future Lichess game archive and training-data boundary

Status: architecture reserved; no current runtime or installation authority.

Lichess Bot is the sole capture and export owner for games it plays. A future implementation should persist one provider-owned canonical game record and derive both Manager history/replay projections and optional training exports from that record. Manager and training consumers must not read the provider's SQLite database, logs, or private runtime files, and the provider must not emit CCR or import Restaurant internals.

The reserved portable export is an immutable, content-addressed, Zstandard-compressed bundle:

```text
manifest.json
games.pgn.zst
positions.jsonl.zst
```

`games.pgn.zst` carries complete terminal games for replay and interoperability. `positions.jsonl.zst` carries one UTF-8 JSON object per pre-move position and deliberately follows the Lichess evaluation-database envelope: a four-field top-level `fen`, optional genuine Lichess-compatible `evals`, and an additive versioned `arena` namespace. The namespace retains the full six-field FEN, remote game id, ply, played UCI move, actor, clocks when available, result, termination, immutable engine/runtime provenance, and optional typed search evidence. MCGS simulations, WDL, proof state, or sparse legal-move policy remain Arena extensions and never masquerade as Lichess depth, knodes, centipawn, mate, or PV fields.

The bundle manifest binds the schema major, provider and export-policy identity, source range, counts, byte sizes, and member digests. Bundles are append-only segments: publication is atomic, retries deduplicate by remote game identity plus content identity, and a partial or invalid bundle never advances the provider-owned cursor. Training exports remove or pseudonymize usernames by default and never contain credentials, token paths, chat, headers, IP data, or raw logs. Any later training reuse has a separate explicit policy gate for applicable terms, licensing, privacy, retention, deletion, and redistribution; this architecture grants no automatic reuse authority.

An external training system such as Restaurant consumes this artifact through its own registered source adapter:

```text
Lichess Bot canonical game archive
        -> immutable PGN.zst + JSONL.zst export
        -> Restaurant source adapter / optional offline reanalysis
        -> delivery and preparation
        -> model-declared CCR materialization
        -> training
```

Restaurant owns source eligibility, sampling, weighting, reanalysis, feature and target semantics, CCR selection, and dataset lineage. Raw game outcomes, played moves, and provider search evidence are provenance-bearing observations, not automatically valid value or policy targets. If the active Restaurant contract requires engine evaluations, an offline Restaurant-owned derivation may add genuine `evals` in a new immutable derived stream; it must never overwrite the provider's raw export or relabel game outcomes or MCGS statistics as Stockfish-style evaluation evidence.

Future Manager Live Games, Game History, and Replay surfaces use bounded typed provider projections over the same canonical game identity. UI-specific records cannot become a second game archive, and Installer only preserves declared provider data paths and wires declared contract locations. Installer never interprets games, evaluations, model semantics, training eligibility, or CCR.

## 4. Engine source topology

```text
engine/
  include/uci_arena/
    core/
    search/
    evaluation/
    model/
    inference/
    knowledge/
    diagnostics/
    uci/
  src/
    core/
    search/
    evaluation/
    model/
    inference/
    knowledge/
    diagnostics/
    uci/
    composition/
  tests/
  tools/evidence/
```

Only `src/composition` knows the complete concrete type graph. It selects implementations, injects dependencies, wires lifecycle, registers adapters and options, and maps validated configuration to concrete bricks.

Example composition:

```text
SearchCore       = MCGS
Evaluator        = NeuralEvaluator
ModelAdapter     = LatticeKnight
InferenceBackend = ONNX Runtime
OpeningBook      = Polyglot
Tablebase        = Syzygy
```

A replacement changes its implementation and one registration/composition entry, not unrelated bricks.

## 5. Chess core

`core` owns board state, move representation, legal generation, make/unmake, FEN, repetition, fifty-move state, check, mate, stalemate, position identity, and game termination. It cannot depend on search, evaluation implementations, models, inference runtimes, UCI, Lichess, Evidence Service, or Manager.

Search, evidence tools, diagnostics, and UCI consume core truth; none may create a competing rules authority.

## 6. Formal search boundary

The provider-neutral `SearchCore` boundary owns one coarse-grained call per search lifecycle, not virtual dispatch per node:

```cpp
class SearchCore {
public:
    virtual ~SearchCore() = default;
    virtual SearchResult search(
        const SearchRequest& request,
        SearchServices& services,
        CancellationToken cancellation) = 0;
    virtual void new_game() = 0;
    virtual void position_transitioned(
        const PositionTransition& transition) = 0;
    virtual SearchCapabilities capabilities() const = 0;
    virtual SearchStatus status() const = 0;
};
```

Shared boundary types include search request/result/limits/services/capabilities/status and cancellation. They contain no minimax- or MCGS-private types.

### 6.1 Minimax

The minimax component owns iterative deepening, alpha-beta, aspirations, quiescence, pruning, reductions, move ordering, transposition semantics, histories, worker coordination, time management, PV construction, and telemetry. No MCGS branch belongs in minimax internals.

### 6.2 MCGS

The MCGS component owns persistent graph topology, nodes, edges, expansion, selection, backup, rerooting, persistent top-k state, work tickets, cursors, concurrent work, batch coordination, stale-result protection, publication, drain semantics, time management, and telemetry. Turn sequence is decoupled from search lifecycle: during an active game, search continues until a terminal game/tablebase outcome or explicit lifecycle cancellation, and a confirmed position transition reroots or safely invalidates only incompatible work. No minimax branch belongs in MCGS internals.

### 6.3 Search composition

The host selects `minimax`, `mcgs`, or a future registered search core once. Adding a search core requires its implementation, tests, capabilities, options, and one registration. It must not modify existing search cores, evaluators, chess core, or UCI algorithms.

## 7. Neutral evaluation

The generic evaluator lives under `evaluation`, never `onnx_evaluator.hpp`, and exposes only engine-owned position, request, result, identity, capabilities, telemetry, context, and error types.

```cpp
class Evaluator {
public:
    virtual ~Evaluator() = default;
    virtual Evaluation evaluate(
        const core::Position& position,
        EvaluationContext context) = 0;
    virtual void evaluate_batch(
        std::span<const EvaluationRequest> requests,
        std::span<Evaluation> results) = 0;
    virtual EvaluationIdentity identity() const = 0;
    virtual EvaluationCapabilities capabilities() const = 0;
    virtual EvaluationTelemetry telemetry() const = 0;
};
```

The neutral contract cannot mention ONNX Runtime, TensorRT, CUDA, DirectML, Stockfish subprocesses, model brands, implementation tensor classes, or provider-specific telemetry.

The Evaluation Service owns provider-neutral admission, priorities, tickets, batching, completion leases, cancellation, generation invalidation, duplicate suppression, completed-result cache, backpressure, capacity, and telemetry. It remains independent of concrete inference runtimes.

## 8. Model-semantic adapters

`ModelAdapter` defines what a model means: features, tensor shapes and element types, materialization, orientation, repetition normalization, policy mapping, value perspective, contract artifacts, semantic fingerprint, and admission validation.

Initial adapters include HalfKA, Rookstorm, and LatticeKnight. Model names apply only to those model implementations and their artifacts. Generic runtime and engine infrastructure must not be named after a model.

Generic inference code cannot contain `if model == ...` branches or infer chess semantics from a tensor shape. Adapter selection uses validated contract metadata and a registry.

## 9. Inference-runtime port

Search and model adapters depend on the engine-owned `InferenceBackend`/`InferenceSession` port, not ONNX Runtime:

```cpp
class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual std::unique_ptr<InferenceSession> create_session(
        const ModelRuntimePackage& package,
        const InferenceConfiguration& configuration) = 0;
    virtual InferenceBackendIdentity identity() const = 0;
};

class InferenceSession {
public:
    virtual ~InferenceSession() = default;
    virtual void execute(
        const InferenceBatch& input,
        InferenceBatchResult& output) = 0;
    virtual void request_termination() noexcept = 0;
    virtual InferenceSessionTelemetry telemetry() const = 0;
};
```

### 9.1 ONNX Runtime adapter

The ONNX adapter owns the ORT environment/session, CPU/DirectML/CUDA provider selection and availability, tensor and I/O binding, session options, warm-up, tuning, runtime and hardware identity, driver fingerprinting, ONNX errors, and ONNX telemetry. No code outside the ONNX adapter includes ONNX Runtime headers.

A TensorRT, custom CUDA, native CPU, or remote backend adds one implementation, tests, dependency packaging, and registration. It does not change search, model semantics, Evaluation Service, chess core, UCI behavior, or ONNX code.

### 9.2 Stockfish oracle

Stockfish is an evaluator adapter, not a neural inference backend. The oracle adapter owns persistent UCI process lifecycle, requests, parsing, caching, identity, timeouts, and telemetry. Generic evaluation contracts contain no Stockfish structures.

## 10. Knowledge sources

Opening books and tablebases implement neutral knowledge ports. Polyglot and Syzygy/Fathom adapters own artifact parsing, lookup, local cache, identity, availability, and result translation. Search consumes `KnowledgeResult` and neither calls Fathom nor parses book files directly.

Book Forge produces books; Engine consumes them. Neither crosses the other's implementation boundary.

## 11. UCI, diagnostics, and evidence runtime

The UCI layer owns protocol parsing, option publication/routing, lifecycle commands, search start/stop, position transitions, result formatting, and status projection. Options are registered by their owners: common host, search core, model adapter, inference adapter, and knowledge source. UCI routes options but does not absorb their internal meaning.

Diagnostics consume typed component snapshots and cannot become a hidden global control plane. Expensive checks are debug-gated, sampled, bounded, or outside hot paths.

Engine evidence tools remain source-owned by Engine because they share chess core, contracts, adapters, and exact runtime behavior. They publish the separately installable `uci_arena_evidence.runtime` component. A deployable component does not become a repository until it gains independent lifecycle, multiple unrelated consumers, stable external ABI, or separate source ownership.

## 12. Dependency direction

```text
UCI / composition
        |
        v
Search contract
        |
        v
Evaluation contract ------> Knowledge contracts
        |
        v
Evaluation Service
        |
        v
Model Adapter
        |
        v
Inference Port
        |
        v
Concrete runtime adapter
```

Forbidden directions include:

- core to search, models, or ONNX;
- search to ONNX Runtime or a model-specific adapter;
- model adapter to MCGS or minimax;
- ONNX adapter to a search core;
- Manager to provider internals;
- Installer to repository source/build layout;
- Engine to the Lichess API; and
- provider products to Manager persistence internals.

## 13. Testing and substitution gates

Tests are organized by core, search contract/core/composition, evaluation contract/service/oracle, model adapter, inference contract/adapter/substitution, knowledge, UCI, and integration.

Mandatory substitution proofs:

- `FakeInferenceBackend` proves search, Evaluation Service, and model adapters operate without ONNX Runtime.
- A fake model adapter proves a conforming tensor contract executes without model-specific changes to the inference adapter.
- A fake search core proves UCI/composition can select a new implementation without changing minimax or MCGS.
- A fake provider proves Manager and Installer consume a public service/component contract without provider source knowledge.

Acceptance rules:

```text
New search core:
  implement SearchCore + tests + registration
  existing search cores, evaluators, and chess core unchanged

New model:
  implement ModelAdapter + tests + registration
  generic inference runtimes unchanged

New inference runtime:
  implement InferenceBackend + tests + registration
  search, model semantics, and Evaluation Service unchanged

New external provider:
  publish service/component contracts + register in composition
  provider behavior does not move into Manager

New installable component:
  publish immutable artifact + manifest + provenance
  add composition data; installer control flow unchanged
```

## 14. Product composition and versioning

Suite and component versions are independent:

```yaml
product_release: 2026.08.0
installer:
  minimum_version: 1.3.0
components:
  engine: {id: uci_arena.engine, version: 2.4.0}
  manager: {id: uci_arena.manager, version: 1.8.0}
  lichess_bot: {id: uci_arena.lichess_bot, version: 1.0.0, optional: true}
  book_forge: {id: uci_arena.book_forge, version: 1.2.0, optional: true}
  evidence_service: {id: uci_arena.evidence.service, version: 1.4.0, optional: true}
  evidence_runtime: {id: uci_arena.evidence.runtime, version: 2.4.0}
```

The composition pins compatible immutable releases. Installer engine version describes deployment behavior; it is not forced to equal a suite or component version.

## 15. Governing LEGO review questions

Every architectural change must answer:

1. Who owns this behavior?
2. Is this a product, formal component, adapter, composition, or internal-helper boundary?
3. Can it change without unrelated owners understanding it?
4. Can a conforming replacement enter through registration/composition?
5. Does a shared contract contain implementation-specific language?
6. Has a third-party API leaked into project-owned types?
7. Does registering a new brick require modifying existing implementations?
8. Is a repository split justified by independent ownership and lifecycle?
9. Is a deployable artifact being mistaken for an independently owned source product?
10. Does the change reduce the radius of the next foreseeable change?

The target is:

> Product repositories own independently released products. Formal components own independent domain behavior. Adapters isolate external implementations. Composition selects concrete bricks. Internal code remains direct and domain-appropriate.
