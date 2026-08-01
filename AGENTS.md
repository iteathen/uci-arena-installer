# AGENTS.md

This repository owns the UCI Arena installation and release-assembly product.
It does not own Manager Core, UCI Arena Engine, Book Forge, Evidence Service,
or Lichess adapter behavior.

## Required architecture

- Read `specs/00_READ_FIRST.yaml` and `docs/installer/ARCHITECTURE.md` before
  substantive work.
- Run `node tools/spec-preflight.mjs --task "<task>" --changed-files <paths>`
  before planning, inspection, implementation, or validation.
- Read every spec returned by preflight. Rerun it if touched paths change.
- Update the governing spec before public behavior.
- Components enter only as immutable release artifacts plus versioned manifests
  and provenance. Source-tree paths and provider-private APIs are forbidden.
- Mechanism, configuration, package data, and policy have separate owners.
- Build an explicit plan before mutation. Stage and validate before commit.
- Preserve a working installation and report rollback failure separately.
- The Windows scope launcher delegates to fixed-scope bundles. Personal is
  non-elevated; All users truthfully requires elevation.
- Do not create automatic pull-request or push CI. Local validation is the
  default; release automation must be manual.

## Mandatory validation

For launcher or release-envelope changes:

```powershell
node tools/spec-check.mjs
node --test tools/release/*.test.mjs
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
```

Installer lifecycle changes additionally require the focused `[setup]` native
tests and the synthetic matrix in `docs/installer/TEST_MATRIX.md`.

Commit real code on an off-main branch, push it, and open a draft pull request.
Do not leave dirty worktrees, obsolete build trees, or merged branches.
