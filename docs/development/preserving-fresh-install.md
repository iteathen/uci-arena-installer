# Preserving uninstall and fresh installation

Use this route when changing the program/data volume of a current-generation
Windows installation. It is a quiesced reinstall with producer-owned restoration,
not a live workspace-migration feature. The selected-root contract is in
[`install_layout_v1.yaml`](../../specs/install_layout_v1.yaml); package variables,
repair semantics and public root query are described in
[package path selection](package-layout-selection.md).

1. Verify the complete replacement installer and exact signed component closure
   before uninstalling. Check destination capacity against each provider's
   resource contract; copying only the executable cannot solve a workspace-volume
   capacity failure.
2. Stop each provider through its public lifecycle, join its exact owned process
   tree and observe its startup registration absent. A stopped Task, stale
   heartbeat or missing package receipt alone does not prove process exit.
3. Use normal preserving component/package uninstall. Retain source artifacts,
   canonical archives and failure receipts. Purge and disk cleanup are separate
   operations; reinstall grants neither authority.
4. Copy declared quiesced user artifacts to fresh destinations and verify bytes,
   relevant sidecars and metadata before promotion. Long nested paths and named
   streams require real platform qualification. Destination permissions follow
   the fresh installation's parent policy; source ACLs are not automatically
   transplanted. Retain the originals until restoration is independently verified.
5. Use the producer's restoration/migration operations before installing its
   startup registration. Render fresh configuration against the new installed
   roots. Preserve historical provenance as history, rather than globally
   replacing strings that happen to contain the old drive letter.
6. Install at the selected program/data locations, query the public installed
   root tuple, then verify receipt versions, provider health and artifact
   usability through Manager/provider contracts. Restoration does not grant
   activation authority to an incompatible timing profile or old engine.

Book Forge owns catalog schema migration and relocation of its contained
locators. Its original workspace pathname must be absent before the verified
copy is adopted; preserve the original with an explicitly checked retained
same-parent rename. Evidence Service owns `workspace adopt-locators` for exact
schema3 offline copies. An older Evidence catalog first needs a verified raw
backup, followed by its ordinary public migration; adoption changes only its
declared artifact/attempt locators. Lichess Bot owns `workspace restore` for
canonical games and durable safety state; it excludes leases, reservations,
lifecycle ownership and runtime/startup authority, and restores offline/paused.
These are separate owner operations, not generic database rewrites.

Do not copy credentials, private keys, browser sessions, Manager private state,
old Task definitions, runtime receipts or replacement intents into new active
authority. External credentials remain blind path references. Preserve book,
timing and other knowledge artifacts independently of whether the selected
engine can currently activate them. Keep the bot stopped when that is the
operator's latest instruction; package success is not permission to restart it.

Managed component deployment treats the complete producer output directory as
one validated payload. Generate file inventories and platform packaging metadata
mechanically from that closure; avoid hand-maintained per-asset lifecycle rules.
Package qualification must execute public entrypoints from the staged payload,
including cold operator commands, so source-tree imports cannot conceal a missing
runtime file. Per-file integrity evidence and one atomic payload responsibility
are compatible; neither requires a second custom installer framework.

## Verified E installation and 0.3.60 upgrade

Complete 0.3.59 established the restored E installation after explicit operator
permission preparation of its existing data root. That retained success does not
prove 0.3.59 automatically secures a fresh custom root. The original C data and
historical repair/failure receipts remain retained.

On 2026-10-11, the reviewed Complete **0.3.60** offline development installer
upgraded that already-prepared per-user installation and joined with exit 0.
Its artifact review decision was `APPROVED_UNSIGNED_COMPLETE_DEVELOPMENT_ARTIFACTS`;
this is installed development acceptance, not signed/stable suite promotion.
The eight signed component versions remain unchanged from 0.3.59.

Public installed checks verified:

- Eight components and all 990 inventoried files at their selected E roots.
- Installer runtime 0.2.3 and all 84 runtime files, plus the facade/scope mapping.
- Public layout: `E:/UCI Arena/Programs/Manager` integration and
  `E:/UCI Arena/Data` data, with install/cache/workspace/knowledge projections
  from that same authority.
- Manager cold host joined its predecessor, doctor returned `ok: true`, and the
  same two previously registered engine IDs remained present.
- Bot remained stopped: startup absent, scheduler disabled and explicitly paused,
  180+0 configuration, 713 archived games/51128 plies, zero leases/active work.

This actual upgrade used an existing safe data root. Automatic restricted
fresh-root creation and unsafe-existing-root refusal have separate focused
native fixture evidence; the upgrade must not be described as a real fresh-root
creation test. The 0.3.59 targeted Vector readiness/memory/deadline observations
remain their original bounded evidence. This upgrade neither reran GPU tests nor
established new clock support, population timing laws, live Bot play or strength.

Retained receipts are `fresh-e-suite0360-install-20261011-execution.json`,
`fresh-e-suite060-installed-inventories-20261011.json`,
`fresh-e-suite060-installed-runtime-20261011.json`,
`fresh-e-suite060-installed-layout-20261011.json`,
`fresh-e-installed-manager-host060-acceptance-20261011.json`,
`fresh-e-manager060-doctor-20261011.json`,
`fresh-e-manager060-engines-20261011.json` and
`fresh-e-suite060-bot-stop-acceptance-20261011.json`.
The executed offline installer SHA-256 is
`089ed1ce78ae08ee887f11bd9ad1b011ad1ce03b49db6c0fcd8f1b63620eab67`;
its retained artifact-review packet SHA-256 is
`145d725a661dd2bf6d4617a6e43f274909c091952bdb9525bbf47616e5550969`.
