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

`arena_suite_release_lock_v2` binds one realization to the exact composition
filename and SHA-256. Provider registrations are keyed data whose repository
must match the composition; they are not a hardcoded list in the validator.
Each release-ready component has either one portable candidate or both Windows
and Linux candidates, and every candidate repeats the composition-owned version
and dependency edges exactly. The checked-in lock may remain an incomplete
`release_ready: false` review draft, but only validation and explicit dry-run
inspection may consume that state. Mutation and publication reject it.

`tools/release/release-lock.mjs` owns structural and composition validation.
`tools/release/mirror-provider-assets.mjs` owns private transport mirroring and
retains the original repository, release tag, asset digest, and provenance
identity for every byte. The mirror never becomes component authority.

Node.js and Stockfish are externally sourced managed components rather than
provider products. Installer owns their exact upstream-input policy,
reproducible packaging, native smoke validation, and keyless provenance. Their
build never reads Manager or provider source trees.

Large operator-owned books, Syzygy collections, and timing evidence are
discovered through a bounded platform search-index adapter. Windows Search and
the Linux locate database provide paths without a recursive drive walk. Content
inspectors catalog standalone books, partial WDL/DTZ collections, and parseable
legacy or incomplete timing evidence with their observed capabilities. A
version or identity mismatch does not block discovery or unrelated components;
exact qualification is required only when a consumer activates a profile or
makes a protected-live safety claim. Indexed `.bin` candidates receive a small
Polyglot framing and ordering sample to filter unrelated binaries without
requiring sidecars, a publication manifest, or a package version. Discovery
records an external in-place
candidate only; explicit import remains the sole locator-registration step.

A published opening-book snapshot owns its artifact names through the bounded,
path-safe `artifacts` map in `snapshot.manifest.json`. Inventory checks the
declared files are present and exposes partial or invalid state without hashing
an arbitrarily large book during discovery. Reviewed import and staged
activation validate the exact declared digests before granting managed
authority. Installer mechanism code must not carry a provider-specific opening-
book filename list.

Installable components publish discoverable resources through the bounded
`discovery` array in `arena-component.json`. Each entry declares a resource kind
and locates it at the active root, a named digest-verified entrypoint, or a safe
component-relative path. Receipt and provider-root inspection consume that data
literally; component ids and filename prefixes never select a resource kind.
The declaration is read-only evidence and provides no activation or lifecycle
authority.

Installable knowledge components separately publish an `activation` array. Each
entry declares `kind`, a safe component-relative `source_path`, the fixed
`knowledge_root` destination authority, and a safe relative `destination_path`.
The source must contain digest-listed payload, the destination must remain below
the Installer-owned knowledge root, resource kinds and destinations must be
unique, and the same resource must have a matching component-path discovery
entry. A component name, filename prefix, or consumption capability never grants
write authority.

Before the first knowledge destination changes, the transaction journal records
every candidate, prior-destination backup, and expected prior state. Each atomic
swap is checkpointed. Receipts project the committed result through
`activated_resources` (`kind`, absolute `path`, and exact `activated_files`) and
retain the schema-v1 flat `activated_files` union for removal compatibility.
Inventory and receipt discovery use the resource declaration rather than the
component id. Normal uninstall preserves these files; explicit purge remains
digest-aware and ownership-bounded.

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
  `external_dependency`, and `unknown_policy_controlled`.

Provider workspaces and continuity state are `user_preserved`. External token
files, in-place books, tablebases, engines, and profiles are
`external_dependency` with purge forbidden. Managed-copy and generated files
use exact identity. This is declarative package knowledge; uninstall and purge
mechanisms contain no Book Forge, Evidence, Lichess, or Manager deletion
branches.

Legacy receipt `data_paths` and the schema-v1 flat `activated_files` projection
are projected into the same ownership contract when preservation evidence is prepared. New
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

## Provider-owned installation integration

An installable provider that needs generated configuration declares one
`arena_provider_installer_integration_v1` object. It names a payload-owned
renderer entrypoint, the exact dependency runtime used to invoke it, its
workspace, whether disabled installation still produces safe configuration,
and bounded dependency and saved-locator bindings.

Installer passes one `arena_provider_install_context_v1` document. The context
contains only receipt-verified roots or entrypoints and validated locator paths;
it never contains credential contents. The provider returns one closed
`arena_provider_install_result_v1` document containing its configuration and
at most sixteen workspace-relative JSON documents. Provider field meanings,
model semantics, profile compatibility, and default policy remain inside the
provider renderer.

Declaring `manager_integration_api` also requires a payload-owned service
contract. This rule is role-based: Installer never uses a provider name,
repository, or component-version threshold to decide whether a contract is
required. `installer_integration` alone may configure a non-service consumer
and does not imply Manager-service ownership. When Manager API metadata is
declared, the component and service-contract projections must match exactly.

Installer owns invocation, path validation, atomic writes, journaling,
verification, and reverse-order rollback. Saving a locator reruns only the
installed renderers that declared that locator kind. Thus a new provider or a
new provider-private configuration field changes package data and provider
code, not Installer component-id branches.

Startup-capable providers add `arena_provider_startup_control_v1` to the same
integration object. It declares a digest-verified entrypoint, fixed install and
uninstall argument arrays, a workspace-relative state path, the configuration
flag, response envelope, and which install-time evidence must be true. Installer
uses those fields literally and passes argument arrays without a shell. It does
not infer Node.js, CLI names, runtime-directory conventions, or provider health
fields. A credential-gated provider still owns authenticated startup install;
its credential-free uninstall declaration lets normal removal revoke startup
authority without exposing a credential to Installer.

Before mutation, validation requires:

- supported schema major and target;
- safe relative paths without symbolic-link escape;
- exact file size and SHA-256 identity;
- declared entrypoints naming manifest-owned files;
- dependency and capability consistency;
- integrated-provider service contract consistency;
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
