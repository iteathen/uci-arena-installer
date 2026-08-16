# UCI Arena Installer Agent Contract

This repository owns UCI Arena suite composition, installation, upgrade/repair/uninstall, release assembly, staging, validation, recovery, and operator-facing deployment results. It does not own Engine, Manager, Lichess Bot, Book Forge, or Evidence Service behavior.

## Required start

1. Read `.agents/core-standard.md`.
2. Before substantive planning, inspection, implementation, or validation run:

```text
node tools/spec-preflight.mjs --task "<task>" --changed-files <likely paths>
```

3. Read every active spec returned by preflight; rerun when touched paths change materially.
4. For repository ownership, component contracts, composition, or cross-repository dependencies, also read `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md` and `specs/suite_lego_boundaries_v1.yaml`.
5. For Installer hardening issues #6-#10, #15, #51, #76-#80, or any delegated implementation packet derived from them, also read `docs/architecture/INSTALLER_HARDENING_ARCHITECTURE.md` and `docs/architecture/INSTALLER_DELEGATION_CONTRACT.md`.

Do not preload historical release notes, retired installer approaches, provider implementation docs, or unrelated specs.

## Design hierarchy

Apply design doctrine in one strict nesting order:

```text
LEGO -> SOLID -> CUPID -> KISS
```

KISS is always inside CUPID, CUPID is always inside SOLID, and SOLID is always inside LEGO. A lower layer may refine only choices that already satisfy every higher layer; it may never override ownership, responsibility, interface, dependency, lifecycle, safety, or truth boundaries fixed above it. A delegated implementation agent reports `blocked_architecture` instead of routing around this hierarchy.

## Authority and ownership

`specs/00_READ_FIRST.yaml` is the installer foundation. The complete suite LEGO architecture owns cross-repository product boundaries. Current installer/component contracts own deployment semantics. Historical plans and provider source layouts are not installation authority.

Installer consumes independently released components only through immutable artifacts, versioned manifests/contracts, checksums/provenance, and declared compatibility. Provider source trees, build trees, private APIs, and product-name-based mechanism branches are forbidden inputs.

Before adding a new installation mechanism, package rule, compatibility path, validator, state store, or recovery flow, search the current installer owner/contracts first. A new conforming component should require composition data and focused tests, not control-flow rewrites.

## Installation safeguards

Installation lifecycle changes are high-consequence operations. Preserve these rules:

- inspect and validate identity/compatibility before mutation;
- create an explicit install plan before filesystem/service mutation;
- stage and validate before replacement;
- preserve user-owned/pre-existing data and the last working installation;
- make rollback and rollback failure visible;
- keep repeated execution idempotent and partial state understandable;
- keep mechanism, configuration, package data, and product policy separately owned;
- Personal scope remains non-elevated; All users truthfully requires legitimate Windows elevation;
- never leave two Manager/provider daemon authorities active for one installation scope;
- never read, print, copy, hash, package, log, or place credential contents in arguments/receipts.

A small reversible documentation-only change does not require release-grade lifecycle ceremony; use the proportionality rules in `.agents/core-standard.md`.

## Validation and release

Validate locally first. Use the preflight-selected owner/contract checks plus the applicable installer matrix. Launcher/release-envelope work uses the documented native/release checks; lifecycle changes additionally require the focused setup tests and synthetic lifecycle matrix.

Automatic pull-request/push CI remains disabled. Release activity is explicit. CI is confirmation, not an editor or exploratory debugger.

## Execution and integration

For code changes, modify tracked source on an off-main branch, include proportionate tests/evidence, inspect actual effects, commit, push, and open a draft PR. Do not leave dirty worktrees, obsolete build trees, or ambiguous staged state.

Before merge, review the complete diff, freeze the intended head, revalidate the exact head/target/checks, use an expected-head guard where supported, and verify target integration.

## Completion

Complete when the requested deployment outcome is evidenced, component ownership remains clean, a working/recoverable installation is preserved where applicable, triggered safeguards pass, and no material invalid partial state remains.
