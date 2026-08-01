# UCI Arena Installer

Private composition, installation, repair, upgrade, and recovery subsystem for
the UCI Arena suite.

This repository is the assembly owner. Component repositories publish immutable
artifacts and self-describing installation manifests; this repository selects
compatible versions and installs them without importing provider source trees.

Automatic CI is disabled. Validation runs locally and release workflows, when
added, must remain explicit `workflow_dispatch` operations.
Transactional cross-platform installer and release assembly for UCI Arena
