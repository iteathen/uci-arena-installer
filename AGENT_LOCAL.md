# Repository context: uci-arena-installer

Universal engineering and design guidance comes from the account-global `AGENTS.md`.

## Mission and ownership

This repository owns UCI Arena suite composition, installation, upgrade/repair/uninstall, release assembly, staging, validation, recovery, and operator-facing deployment results. It does not own Engine, Manager, Lichess Bot, Book Forge, or Evidence Service behavior.

## Local routing

Before substantive work run:

```text
node tools/spec-preflight.mjs --task "<task>" --changed-files <likely paths>
```

Read the active specs returned by preflight. Cross-repository ownership/composition follows `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md` and `specs/suite_lego_boundaries_v1.yaml`. Installer hardening/delegation work also follows `docs/architecture/INSTALLER_HARDENING_ARCHITECTURE.md` and `docs/architecture/INSTALLER_DELEGATION_CONTRACT.md` when applicable.

Move-time ownership follows `docs/architecture/UCI_ARENA_MOVE_TIME_AUTHORITY.md`.
The hardening document distinguishes future stable cross-platform architecture
from the implemented current-generation package/layout path; do not claim the
target is complete or revive cancelled C++ chess/search.

## Local constraints

Installer owns deployment/composition outcomes, not component behavior. Cross-repository interactions use declared component/release contracts rather than private sibling implementation paths.
