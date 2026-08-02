# UCI Arena Installer

Private composition, installation, repair, upgrade, and recovery subsystem for
the UCI Arena suite.

This repository is the assembly owner. Component repositories publish immutable
artifacts and self-describing installation manifests; this repository selects
compatible versions and installs them without importing provider source trees.

Automatic CI is disabled. Validation runs locally and release workflows, when
added, must remain explicit `workflow_dispatch` operations.

Normal uninstall removes installed software authority while preserving valuable
state. Preview it with `arena-setup uninstall-plan --all`; apply it with
`arena-setup uninstall --all --yes`. Destructive removal is a separate flow:
write and review `arena-setup purge-plan --component ID --result-file plan.json`,
then apply that exact plan with
`arena-setup purge --plan-file plan.json --yes`.

Purge is path-bounded and ownership-aware. It never deletes external token
files, in-place books, tablebases, engines, or profiles, and it preserves and
reports a managed file whose current identity differs from its receipt.
