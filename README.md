# UCI Arena Installer

Private composition, installation, repair, upgrade, and recovery subsystem for
the UCI Arena suite.

This repository is the assembly owner. Component repositories publish immutable
artifacts and self-describing installation manifests; this repository selects
compatible versions and installs them without importing provider source trees.

## Steady-state invariant

The target Installer is a stable mechanism layer. A routine application release
that remains within existing installation contracts changes component artifacts,
manifests, provenance, composition data, or versioned suite media rather than
Installer control flow.

In steady state, an ordinary Engine, Manager, or provider application update
must not require a new native Installer build merely because application bytes,
versions, internal file layout, or manifest-declared relative entrypoints changed.
It must not require Windows MSI/Burn/bootstrap recompilation or Linux stable
integration/bootstrap recompilation for application-only changes.

Windows and Linux use different platform adapters, but they share the same
Installer semantics: exact immutable component identity, explicit composition,
exact persisted installed-layout authority, bounded acquisition/staging, one
transaction/rollback truth, manifest-declared lifecycle/resource behavior, and
stable OS integration separated from replaceable application payloads.

Generic Installer mechanism code must not select behavior from current product
names such as Engine, Lichess, Book Forge, Evidence, opening books, tablebases,
or timing profiles. Those names may exist in composition/component metadata and
operator-facing labels. A new conforming component should require data and tests,
not a new Installer control-flow branch.

The repository is still transitioning to this state. The cross-platform
acceptance gate is issue #161. Linux persisted-layout parity is #162, stable Linux
Manager integration is #163, and product-neutral setup/resource planning is #164.
Windows layout/integration work remains #80/#77, deterministic release planning
is #78, and stable fresh-install media/bootstrap is #79. Legacy product-aware,
raw-root/current-default, and per-application integration/bootstrap paths are
transitional rather than target authority.

Automatic CI is disabled. Validation runs locally and release workflows, when
added, must remain explicit `workflow_dispatch` operations.

## Current package-only update path

Routine provider and Engine component releases can use the existing package-only
path without rebuilding Manager MSI, Burn, or the native Installer runtime:

```powershell
npm run release:package-update -- --platform windows-x86_64 --component COMPONENT_ID --lock LOCK --asset-root ASSETS --output-root OUTPUT
```

The output archive contains a fixed `apply-update.ps1`, the exact catalog, and
digest-verified payloads. This is a useful current low-level path, not the final
operator workflow. Issue #78 replaces manual coordination of lock, composition,
assets, metadata, targets, and output with one deterministic release-input plan
consumed by package-only and fresh-install media builders.

Full stable integration/bootstrap rebuilds are reserved for genuine Installer
protocol, trust-bootstrap, layout/integration, or other installation-mechanism
changes. An application-only release must not be classified as such merely
because its internal bytes or paths changed.

On private repositories without hosted-runner capacity, the checked-in local
ephemeral runner command performs all version, workflow, toolchain, Git Bash,
PowerShell module, signing, label, clean-tree, and exact-remote-head checks
before registering one update-disabled runner and dispatching exactly one
workflow attempt. Use `-PreflightOnly` first; logs remain under ignored
`build/release-runner-logs`.

Normal releases require `-Ref main` and the exact remote `main` head. A branch
is rejected unless `-AllowUnmergedDiagnostic` is explicit; that override marks
the result diagnostic-only and gives it no publication or composition authority.
Review and merge the exact PR head before running a normal component release.

## Removal

Normal uninstall removes installed software authority while preserving valuable
state. Preview it with `arena-setup uninstall-plan --all`; apply it with
`arena-setup uninstall --all --yes`. Destructive removal is a separate flow:
write and review `arena-setup purge-plan --component ID --result-file plan.json`,
then apply that exact plan with
`arena-setup purge --plan-file plan.json --yes`.

Purge is path-bounded and ownership-aware. It never deletes external credential
files or other operator-owned resources merely because they are adjacent to an
Installer-owned root, and it preserves and reports managed content whose current
identity differs from reviewed receipt/evidence.
