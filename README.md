# UCI Arena Installer

Private composition, installation, repair, upgrade, and recovery subsystem for
the UCI Arena suite.

This repository is the assembly owner. Component repositories publish immutable
artifacts and self-describing installation manifests; this repository selects
compatible versions and installs them without importing provider source trees.

Automatic CI is disabled. Validation runs locally and release workflows, when
added, must remain explicit `workflow_dispatch` operations.

Routine provider and Engine component releases use the package-only path and
do not rebuild Manager MSI, Burn, or the native Installer runtime:

```powershell
npm run release:package-update -- --platform windows-x86_64 --component COMPONENT_ID --lock LOCK --asset-root ASSETS --output-root OUTPUT
```

The output archive contains a fixed `apply-update.ps1`, the exact catalog, and
digest-verified payloads. Full installer composition is reserved for Installer
protocol, Manager MSI, launcher, or trust-bootstrap changes.

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

Normal uninstall removes installed software authority while preserving valuable
state. Preview it with `arena-setup uninstall-plan --all`; apply it with
`arena-setup uninstall --all --yes`. Destructive removal is a separate flow:
write and review `arena-setup purge-plan --component ID --result-file plan.json`,
then apply that exact plan with
`arena-setup purge --plan-file plan.json --yes`.

Purge is path-bounded and ownership-aware. It never deletes external token
files, in-place books, tablebases, engines, or profiles, and it preserves and
reports a managed file whose current identity differs from its receipt.

Fresh Windows installations can select program and data locations through the
bundle's `ArenaProgramRoot` and `ArenaDataRoot` variables. Repair retains the
installed selection; changing location uses preserving uninstall and a fresh
installation. Query the installed authority with
`arena-setup --install-scope per-user --json layout`, then forward its complete
five-root tuple. See [package path selection](docs/development/package-layout-selection.md)
and [preserving fresh installation](docs/development/preserving-fresh-install.md).
The current-generation MSI record does not claim the pending IL05/06/07 or
transaction-v2 architecture is complete.
