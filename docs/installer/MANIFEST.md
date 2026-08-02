# Package manifest

`arena_product_composition_v1` is the Installer-owned, platform-neutral suite
bill of materials. It declares the product release, minimum installer version,
component roles, exact component versions, provider repositories, selection
policy, and dependency edges. It contains no source-tree paths or platform
artifact guesses. A platform release lock must realize every selected
composition entry with immutable artifact, component-manifest, digest, and
provenance evidence.

The composition and installer engine are versioned independently. Changing a
provider version or adding a conforming provider changes composition data and
its lock realization, not transaction control flow.

Node.js and Stockfish are externally sourced managed components rather than
provider products. Installer owns their exact upstream-input policy,
reproducible packaging, native smoke validation, and keyless provenance. Their
build never reads Manager or provider source trees.

`arena_install_catalog_v1` selects immutable components. Each `arena_install_component_v1` manifest is the authority for component identity, target, payload files, entrypoints, dependencies, capabilities, startup ownership, public service contract, data paths, and default model metadata.

Catalog presets and explicit custom selections are the only ordinary component-selection authorities. Named `optional_component_groups` map operator requests to exact component ids. Per-component `default_enabled`, `disable_when_workspace_capacity_unavailable`, and `activation_role` fields control planning without product-name or id-prefix branches. Required suite members therefore belong in a preset or dependency edge; the planner never injects one implicitly.

An optional `feature_selection` object defines the base preset, exact operator-visible feature component ids, and default selections used by Windows Modify/update flows. The engine computes both the selected dependency closure and removable controlled closure from that object. Executables and WiX command lines carry only `component-id=0|1` assignments; neither contains a duplicate component list.

`activation_role: knowledge` means installed bytes also consume capacity in the knowledge activation root. It does not define deployment paths or locator semantics; those remain provider-manifest installation data validated before activation.

Changing package contents normally changes catalog or component data, not orchestrator control flow. Values must not be duplicated in batch launchers, PowerShell scripts, WiX source, or C++ branches.

## Ownership projection

Each installation receipt may project manifest-owned paths into an `ownership`
array. Every entry declares an absolute path, ownership class, normal-uninstall
decision, purge policy, and path kind. Exact-identity managed files also carry
size and SHA-256. The authoritative policy values are:

- `normal_uninstall: remove|preserve`;
- `purge: explicit_recursive|exact_identity|empty_only|forbidden`;
- ownership classes `application_replaceable`, `user_preserved`, `generated`,
  `migrated`, `cache_disposable`, `installer_temporary`,
  `external_dependency`, and `unknown`.

Provider workspaces and continuity state are `user_preserved`. External token
files, in-place books, tablebases, engines, and profiles are
`external_dependency` with purge forbidden. Managed-copy and generated files
use exact identity. This is declarative package knowledge; uninstall and purge
mechanisms contain no Book Forge, Evidence, Lichess, or Manager deletion
branches.

Legacy receipt `data_paths` and `activated_files` are projected into the same
ownership contract when preservation evidence is prepared. New
receipt-producing installation paths emit explicit ownership so future
preservation changes remain data changes rather than removal-control-flow
branches.

Provider products may publish a richer product-owned manifest under
`contracts/component-manifest.json`, but every installable archive also carries
one generated root `arena-component.json`. That root document is the
Installer-facing inventory: it binds every payload file by relative path, size,
and SHA-256 and projects only the entrypoints, dependencies, startup ownership,
data paths, and public contract references needed by installation. It is
generated from provider-owned package data; Installer does not translate
repository layouts or infer provider behavior.

`uci_arena_service_contract_v1` providers additionally publish the exact
Manager operation catalog and configuration/status schemas referenced by their
service contract. Installer validates and receipts those files as opaque typed
contracts. It does not interpret operation meaning, open provider storage, or
handle credential contents.

The same contract declares whether its CLI is a direct executable or uses one
named dependency runtime and entrypoint. A dependency-runtime declaration must
match a component dependency; executable suffixes and provider identities are
never interpreter-selection policy.

Before mutation, validation requires:

- supported schema major and target;
- safe relative paths without symbolic-link escape;
- exact file size and SHA-256 identity;
- declared entrypoints naming manifest-owned files;
- dependency and capability consistency;
- standalone service contract consistency where required;
- sufficient capacity for cache, stage, installation, backup, and operational reserve;
- and signed provenance for official or complete-development composition.

Configuration contains operator and environment values such as roots, feature selection, offline mode, timeouts, and development trust. Policy contains decisions such as downgrade rules, preservation, repair, backup, process transitions, and valid-state requirements. Neither belongs in the launcher.

## Windows launcher envelope

`arena_windows_installer_envelope_v1` is a deterministic binary release-assembly
contract: launcher stub bytes, signed Personal bundle bytes, signed All users
bundle bytes, then a fixed 128-byte little-endian footer. The footer contains a
16-byte identity, version and reserved words, stub size, each payload's offset
and size, and each payload's 32-byte SHA-256. Payloads must be nonempty,
contiguous, and end exactly at the footer. The launcher validates both digests
before asking for or applying a scope choice.

Online and offline envelopes are built separately from explicit release inputs.
Changing a version or child filename changes release assembly data, not native
launcher control flow.
