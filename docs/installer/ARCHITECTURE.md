# Installer architecture

The UCI Arena installer is a product-specific assembly of replaceable bricks. `arena-setup` is the native transaction engine; Windows bundles and Linux packages are launch and lifecycle adapters; the release catalog and component manifests contain package knowledge.

The stable dependency flow is:

```text
Launcher -> Orchestrator -> Configuration + Manifest
                         -> Environment + Installation inspection
                         -> Planner -> Staging -> Deployment
                         -> Validators -> Commit
                         -> Backup + Rollback on failure
                         -> Structured log and result
```

No brick may discover a neighboring implementation. Inputs are explicit, paths are absolute and normalized at boundaries, external commands use argument arrays, and secrets are redacted or represented only by external path references.

The scope-neutral Windows launcher is only a front door. It selects Personal or All users and delegates to the matching fixed-scope bundle. The fixed bundles retain separate product identities, cache identities, privilege models, install roots, data roots, PATH ownership, and Manager lifecycle. The launcher never becomes an installer engine.

Windows release assembly is Installer-owned. The Manager repository publishes
one immutable `manager.core` artifact with a root component manifest; it does
not publish or author the suite MSI or Burn bundle. The Manager MSI builder
validates every manifest-owned byte and applies the declarative
`manager-integration.json` policy for fixed roots, PATH, shortcut, scope marker,
and the machine-only service. The suite-bundle builder consumes that MSI, the
Installer-owned `arena-setup` runtime, and exact catalog media. Neither builder
accepts a Manager source or CMake install tree.

Package content, entrypoints, dependencies, health checks, preservation, and uninstall ownership belong to `arena_install_catalog_v1` and `arena_install_component_v1`. Policy belongs to the installation contract and versioned configuration. Mechanism belongs to installer components.

Large external chess assets deliberately use a different compatibility rule
from executable components. Discovery and preservation are version-permissive:
standalone books, partial Syzygy collections, and parseable timing evidence stay
visible even when optional metadata, suite versions, or runtime identity differ.
Consumers enforce only the capabilities needed for a requested activation and
must retain incompatible timing evidence as cataloged evidence with an explicit
migration or requalification reason.

Removal is implemented as a separate `RemovalLifecycle` brick behind the
`SetupEngine` façade. Its contract separates `plan_uninstall` from `uninstall`
and `plan_purge` from `purge`. Provider startup behavior enters through explicit
hooks supplied by the composition façade; removal code does not discover or
interpret a provider implementation. The same brick is directly testable with
fake hooks and synthetic receipts.

The governing specification is `specs/installation_contract_v1.yaml`. The remaining files in this directory define the focused contracts agents must read before installer work.
