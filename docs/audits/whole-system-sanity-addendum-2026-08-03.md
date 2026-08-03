# UCI Arena Whole-System Sanity Audit Addendum — 2026-08-03

## Purpose

This addendum extends `whole-system-sanity-2026-08.md` with findings produced by
the post-LEGO-boundary audit. It is intentionally stored with the Installer
release-assembly branch because the Installer owns the complete suite
composition and is the only repository that can evaluate cross-repository
compatibility as one product.

This document is an audit ledger, not an implementation plan. A correction is
not considered closed merely because a branch or pull request exists. Closure
requires the owning repository's specified local checks plus the Installer's
clean packaged-suite acceptance cycle.

## Audit method

The review is split as a tree rather than one linear code pass:

1. Suite authority and product identity.
2. Repository ownership and substitution boundaries.
3. Public contracts and composition manifests.
4. Product-internal components.
5. Persistence, lifecycle, cancellation, failure, and recovery paths.
6. Build, release, installation, repair, rollback, and removal.
7. Cross-component failure interactions.
8. Tests, evidence, and falsifying observations.

Each audit question receives exactly one status:

- `answered`: sufficient source/spec/test evidence supports the expected result.
- `contradiction`: implementation, active documentation, or ownership conflicts
  with the governing contract.
- `unknown`: available evidence is insufficient; this is never counted as a
  pass.
- `not_applicable`: the question is outside that component's declared purpose,
  with the owning component named where possible.

## Current question ledger

| Question ID | Question | Status | Evidence / disposition |
|---|---|---|---|
| Q-GOV-001 | Do active Engine agent instructions and specification indexes identify only UCI Arena Engine? | contradiction | Engine `AGENTS.md`, AI context, and active registry contained legacy product authority. Corrected in UCI_Arena draft PR #40; local spec preflight and merge validation remain required. |
| Q-GOV-002 | Are all active suite documents free of legacy product identity? | contradiction | Evidence Service active resource contract and README retained legacy foreground-owner wording. Contract/config/doc correction is in Evidence draft PR #11. Physical unregistered legacy documents in Engine remain a separate cleanup inventory and are not silently treated as removed. |
| Q-CORE-001 | Does the canonical chess-state parser reject malformed FEN fields completely? | contradiction | `Position::from_fen` accepts trailing numeric text, zero-length board runs, invalid en-passant ranks, and trailing fields. Repair is not yet integrated because the connector cannot patch or compile the large private translation unit safely. |
| Q-CORE-002 | Can castling be generated or executed without the required rook physically present? | contradiction | Castling generation trusts rights bits without verifying the rook square; make/unmake then synthesizes a rook. This violates canonical legality ownership and remains open. |
| Q-MANAGER-001 | Is suite installation and release composition owned only by Installer? | contradiction | Manager main still contains active setup/release assembly authority. Existing Manager draft PR #54 removes it while Installer draft PR #2 receives release assembly. Closure requires the complete packaged release cycle. |
| Q-LICHESS-001 | Can the Lichess product support standard UCI engines without pretending they expose UCI Arena extended capabilities? | contradiction | The previous live contract made generic substitution theoretical. Lichess draft PR #3 introduces separate standard and extended engine adapters; integration validation remains required. |
| Q-BOOK-001 | Does Book Forge reject unknown, unbounded, contradictory, or unsafe configuration before component behavior starts? | contradiction | Broad PR #8 added first-layer hardening; stacked Book Forge draft PR #11 adds the exact shape/range/path contract, cross-field checks, physical secret-path ownership, schema parity, and focused tests. |
| Q-EVIDENCE-001 | Does Evidence CLI syntax fail closed before configuration, catalog, credentials, or provider behavior? | contradiction | Previous parser overwrote duplicate flags, accepted extra positionals/unknown flags, consumed flag-shaped values, prefix-parsed integers, and accepted unbounded JSON. Corrected in stacked Evidence draft PR #13. |
| Q-EVIDENCE-002 | Is dataset file export memory bounded by one observation rather than the complete dataset? | contradiction | Previous export called `Catalog.dataset()`, loaded all datasets/observations, and joined the complete body. Corrected in stacked Evidence draft PR #14 with a read-only snapshot port and atomic streaming export. |
| Q-EVIDENCE-003 | Is direct `dataset show` output memory bounded without changing its JSON contract? | contradiction | Previous path materialized the complete dataset and JSON document. Corrected in stacked Evidence draft PR #15 through a validated disposable spool and bounded stdout streaming. |
| Q-RELEASE-001 | Has one clean Windows and Linux component-to-suite release/install/repair/upgrade/rollback/remove cycle passed with the new ownership split? | unknown | Branch and contract evidence exist, but the final packaged cycle has not been observed in this audit environment. |
| Q-TEST-001 | Have the new provider repairs passed their required Node 24.15 local checks? | unknown | This audit environment cannot clone the private repositories and has a lower Node version. Required commands are recorded on each PR; absence of execution is not a pass. |
| Q-COVERAGE-001 | Has every current source line in every repository been enumerated and reviewed? | unknown | The GitHub connector does not expose private directory trees or clone access. Coverage is being derived from specs, manifests, commit paths, and known source indexes; remaining inventory gaps must stay visible. |

## Stable defect register

### ENGINE-CORE-031 — permissive canonical FEN admission

- Category: chess correctness / external input boundary
- Severity: high
- Status: open
- Owner: Engine chess core
- Evidence:
  - integer parsing does not require full-token consumption;
  - board digits admit `0`;
  - en-passant accepts any board square rather than the FEN rank contract;
  - extra fields are ignored.
- Consequence:
  - malformed external state can enter hashing, repetition, legality, search,
    model encoding, and UCI behavior as if canonical.
- Required correction:
  - strict six-field parsing;
  - exact numeric tokens and legal ranges;
  - exact eight-rank board grammar with run digits `1..8`;
  - side-consistent en-passant rank;
  - no trailing tokens;
  - focused rejection tests plus existing perft/make-unmake probes.
- Required validation:
  - build the core probes;
  - run strict FEN fixtures;
  - run make/unmake identity and perft gates;
  - UCI `position fen` rejection smoke.

### ENGINE-CORE-032 — castling without a physical rook

- Category: chess legality / state mutation
- Severity: critical
- Status: open
- Owner: Engine chess core
- Evidence:
  - pseudo-move generation checks rights, path, and attacks but not the expected
    rook piece on `a1/h1/a8/h8`;
  - castling mutation writes a rook to the destination regardless of the source
    square's actual contents.
- Consequence:
  - a malformed or stale position can create material and produce an illegal
    move accepted by every higher layer.
- Required correction:
  - require the same-color rook on the declared rook source in both full move
    generation and fast `has_any_legal_move` generation;
  - make castling mutation assert or fail closed if the source is not that rook;
  - add all four missing/wrong-color/wrong-piece rook fixtures.
- Required validation:
  - make/unmake identity probe;
  - castling legality cases;
  - start-position perft;
  - malformed-FEN UCI smoke.

### EVIDENCE-DOC-033 — legacy identity in active Evidence resource documentation

- Category: governance / provider-neutral contract
- Severity: medium
- Status: corrected on branch, pending validation
- Owner: Evidence Service resource-admission contract
- Correction:
  - draft PR #11 uses `externalOwnerRoots` and generic foreground compute-owner
    language;
  - the prior field remains input-only and non-enumerable for bounded
    compatibility;
  - the README now uses the generic owner concept.
- Required validation:
  - `npm run check`;
  - configuration serialization contains only canonical identity;
  - Book Forge shared-runtime compatibility remains intact.

### BOOK-CONFIG-034 — incomplete Book Forge configuration admission

- Category: configuration / resource safety / credential ownership
- Severity: high
- Status: corrected on stacked branch, pending validation
- Owner: Book Forge configuration component
- Correction:
  - broad draft PR #8 provides the initial system hardening;
  - stacked draft PR #11 adds exact field shape, representative upper/lower
    bounds, cross-field ordering, optional-capability path semantics, physical
    credential ownership checks, published JSON-schema parity, and preflight
    routing.
- Required validation:
  - merge/rebase after PR #8;
  - `npm run check` on Node 24.15+;
  - release package config validation through Manager and Installer;
  - Windows junction and Linux symlink credential-path cases.

### EVIDENCE-CLI-035 — permissive typed CLI boundary

- Category: public command contract / Manager integration
- Severity: high
- Status: corrected on stacked branch, pending validation
- Owner: Evidence Service CLI parser
- Correction in draft PR #13:
  - exact command paths and arity;
  - command-specific flag allowlists;
  - required flags and mutually exclusive job modes;
  - duplicate, unknown, missing, flag-shaped, equals-form, and oversized input
    rejection;
  - strict bounded integers and JSON-object values;
  - help cannot bypass syntax or initialize the catalog.
- Required validation:
  - merge/rebase after Evidence PR #10;
  - `npm run check`;
  - Manager API invocation fixtures;
  - installed component CLI smoke.

### EVIDENCE-EXPORT-036 — whole-dataset export materialization

- Category: persistence / scale / file publication
- Severity: high
- Status: corrected on stacked branch, pending validation
- Owner: Evidence read/export components
- Correction in draft PR #14:
  - read-only catalog adapter with one explicit SQLite snapshot;
  - lazy `iterate()` observation port;
  - strict stored JSON/count validation;
  - one-observation-at-a-time JSON/JSONL serialization;
  - adjacent exclusive temporary file, full-write loop, fsync, close, snapshot
    commit, then atomic rename;
  - count reconciliation and cleanup;
  - catalog/sidecar destination protection.
- Required validation:
  - merge/rebase after PR #13;
  - `npm run check` on Node 24.15+;
  - export a multi-million-observation synthetic dataset while measuring peak
    RSS and verifying exact digest/order;
  - kill/failure tests around write, sync, commit, and rename.

### EVIDENCE-SHOW-037 — whole-dataset direct display materialization

- Category: operator output / scale / stream lifecycle
- Severity: high
- Status: corrected on stacked branch, pending validation
- Owner: Evidence dataset display component
- Correction in draft PR #15:
  - reuse the validated JSON exporter to produce a complete disposable spool;
  - emit no stdout before validation, count reconciliation, sync, and snapshot
    commit;
  - stream bounded file chunks to a replaceable writable without ending it;
  - handle write-callback/error-event ordering;
  - clean the spool on success, missing data, validation failure, and broken
    downstream output.
- Required validation:
  - merge/rebase after PR #14;
  - `npm run check`;
  - large stdout redirection test;
  - broken-pipe test on Windows and Linux;
  - verify no stale display spools after ordinary failure paths.

### MANAGER-INSTALL-038 — duplicate suite installation/release authority

- Category: LEGO ownership / release engineering
- Severity: critical
- Status: correction already in open branches, final cycle unknown
- Owner migration:
  - Manager draft PR #54 removes setup and suite assembly authority;
  - Installer draft PR #2 receives composition, provider retrieval, catalog
    assembly, acceptance, and release ownership.
- Required validation:
  - isolated Manager build contains no setup transaction authority;
  - isolated Installer build can construct the full suite without Manager source;
  - complete install/repair/upgrade/rollback/remove cycle on Windows and Linux;
  - preserved data and credential-reference behavior verified.

### RELEASE-INTEGRATION-039 — final packaged suite proof absent

- Category: cross-repository integration
- Severity: critical gate
- Status: unknown
- Required evidence:
  - exact component candidates and provenance;
  - composition lock and suite catalog;
  - clean isolated build receipts;
  - install, start, health, provider substitution, repair, upgrade, rollback,
    uninstall, and preserved-data checks;
  - no source-tree or build-tree dependencies in the installed suite.

### AUDIT-COVERAGE-040 — private tree enumeration incomplete

- Category: audit completeness
- Severity: process gate
- Status: unknown
- Constraint:
  - connector code search is not indexed for several repositories and private
    tree enumeration/clone is unavailable in this environment.
- Required mitigation:
  - derive inventory from CMake/package manifests, component definitions,
    specification indexes, workflows, release manifests, and all commit paths;
  - compare that inventory with a local `git ls-files` run before calling the
    audit complete;
  - every omitted path must be classified, not assumed harmless.

## Pull-request dependency map

### Engine

- PR #40 — corrected Engine agent/spec governance.
- `agent/fix-canonical-chess-state` — reserved repair branch for
  ENGINE-CORE-031/032; no safe code commit yet.

### Manager / Installer

- Manager PR #54 — remove installer and suite-release authority.
- Installer PR #2 — receive complete release assembly and audit authority.
- Installer PR #3 — globally bound indexed asset results.

### Book Forge

- PR #8 — broad system-sanity foundation.
- PR #11 — stacked exact configuration contract; base is PR #8 branch.

### Evidence Service

- PR #10 — broad system-sanity foundation.
- PR #11 — generic foreground compute-owner contract/documentation.
- PR #12 — active model identity/path confinement.
- PR #13 — strict CLI grammar; base is PR #10 branch.
- PR #14 — streaming atomic dataset export; base is PR #13 branch.
- PR #15 — validated streaming dataset display; base is PR #14 branch.

### Lichess Bot

- PR #3 — standard versus extended UCI engine boundaries and related lifecycle
  hardening.

## Mandatory next audit lanes

1. Complete Engine canonical state repair and executable chess probes.
2. Inspect Evidence catalog list/query caps, prune transactions, and daemon
   lifecycle line by line after the export/display stack.
3. Inspect Book Forge catalog, acquisition frontier, Stockfish worker pool,
   publication, retention, and startup line by line after PR #8/#11.
4. Inspect Lichess HTTP/rate-limit/event-stream/game-session/store code against
   PR #3 contracts, including reconnect races and credential-path confinement.
5. Inspect Manager API/auth/process supervisor/tournament/store/UI composition
   after installer authority is removed.
6. Inspect Installer planner/state detection/staging/deployment/migration/
   rollback/removal and release workflows after PR #2/#3.
7. Run a local `git ls-files` inventory across all six repositories and reconcile
   every path against this ledger.
8. Execute the clean packaged-suite cycle; do not close unknowns from source
   inspection alone.
