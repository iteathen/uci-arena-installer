# Package manifest

`arena_install_catalog_v1` selects immutable components. Each `arena_install_component_v1` manifest is the authority for component identity, target, payload files, entrypoints, dependencies, capabilities, startup ownership, public service contract, data paths, and default model metadata.

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
