# Package manifest

`arena_install_catalog_v1` selects immutable components. Each `arena_install_component_v1` manifest is the authority for component identity, target, payload files, entrypoints, dependencies, capabilities, startup ownership, public service contract, data paths, and default model metadata.

Catalog presets and explicit custom selections are the only ordinary component-selection authorities. Named `optional_component_groups` map operator requests to exact component ids. Per-component `default_enabled`, `disable_when_workspace_capacity_unavailable`, and `activation_role` fields control planning without product-name or id-prefix branches. Required suite members therefore belong in a preset or dependency edge; the planner never injects one implicitly.

An optional `feature_selection` object defines the base preset, exact operator-visible feature component ids, and default selections used by Windows Modify/update flows. The engine computes both the selected dependency closure and removable controlled closure from that object. Executables and WiX command lines carry only `component-id=0|1` assignments; neither contains a duplicate component list.

`activation_role: knowledge` means installed bytes also consume capacity in the knowledge activation root. It does not define deployment paths or locator semantics; those remain provider-manifest installation data validated before activation.

Changing package contents normally changes catalog or component data, not orchestrator control flow. Values must not be duplicated in batch launchers, PowerShell scripts, WiX source, or C++ branches.

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
