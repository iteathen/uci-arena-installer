# Installer test matrix

Automated tests use isolated temporary roots, synthetic manifests, fake packages, controlled state trees, and injectable platform adapters. A test asserts filesystem state, ownership, receipts, logs, backups, rollback, final classification, and exit code.

Required scenarios:

- clean and repeated clean install;
- spaces, Unicode, long paths, trailing separators, and unrelated working directory;
- valid upgrade, invalid downgrade, repair, reinstall, replacement, uninstall,
  and separately planned purge;
- missing or corrupt payload and invalid manifest;
- unsafe, external, missing, size-mismatched, and digest-mismatched retained
  release media rejected before installation mutation;
- missing runtime and failed dependency command;
- locked destination and partial existing installation;
- interrupted stage and staged validation failure;
- failure after backup and during replacement;
- rollback success and rollback failure;
- user-data preservation and application-file replacement;
- uninstall-plan dry-run with no mutation, confirmation enforcement, and stale
  receipt-plan rejection;
- removal of executable/startup/receipt authority while Manager continuity,
  managed-copy books, provider workspaces, profiles, locators, retained media,
  caches, logs, and transaction evidence remain;
- purge preview with no mutation, separate confirmation, root-equality and
  out-of-bounds rejection, symbolic-link/Windows-reparse rejection, external/unknown ownership
  preservation, exact-identity deletion, modified-file preservation and
  reporting, partial failure evidence, and repeated idempotent purge;
- ambiguous, corrupt, duplicate, or filename-mismatched receipt authority
  blocking uninstall planning before mutation;
- recursive purge ownership blocked when it overlaps a protected
  receipt/preservation root or any explicitly external/unknown path;
- retained paths never counting as installed authority and reinstall
  revalidation of preserved state;
- a saved locator for the exact receipt-managed resource preserving the receipt
  as selected installation authority while a different explicit locator wins;
- same-source retained candidates selecting stronger content evidence before
  deterministic path order without treating discovery quality as authority;
- saved knowledge/evidence locators being re-inspected after their external
  content changes, with invalid content remaining visible as broken;
- idempotent configuration, PATH, shortcuts, startup, and migration;
- post-install health success and failure;
- read-only indexed discovery for opening books, partial Syzygy collections,
  timing evidence, and Lichess single-game/capacity/transport profiles through
  typed product-owned query rules and replaceable Windows/Linux adapters;
- unavailable, timed-out, truncated, empty, legacy, incomplete, incompatible,
  and decoy indexed results remaining visible or rejected as appropriate
  without registration, copying, activation, or filesystem mutation;
- explicit root inspection bounded to root/direct-child files, saved-locator
  selection outranking a bundled bootstrap asset, and installed receipt
  authority remaining visible independently of the selected locator;
- arbitrary component identities exercising declarative selection, default
  activation, capacity policy, and knowledge accounting without engine changes;
- declarative knowledge activation with an unrelated component identity,
  misleading knowledge-shaped identity without activation authority, bounded
  source/destination validation, receipt-consistent managed inventory, prior
  destination restoration after later failure, and normal-uninstall
  preservation after receipt and binary authority are removed;
- cleanup after success and failure;
- Manager artifact byte/manifest mismatch and version-drift rejection before
  MSI generation;
- WiX compilation of both fixed-scope Manager MSIs from a synthetic immutable
  component artifact, including user/machine PATH ownership, Personal service
  exclusion, and All-users LocalService registration;
- catalog/policy feature mismatch and secret-bearing suite-media rejection;
- WiX compilation of both fixed-scope suite bundles with default-on persisted
  Modify controls, explicit apply/repair operations, and normal uninstall with
  no purge path;
- Personal delegation without elevation;
- All-users delegation to the elevation-requiring machine bundle;
- deterministic online and offline launcher envelopes, exact embedded bundle
  digests, corrupt-envelope rejection, and temporary-child cleanup;
- installed-scope maintenance and explicit cross-scope block;
- one startup owner with no duplicate Manager, Book Forge, Evidence, or Lichess daemon;
- replaceable provider service contracts with independent operation-catalog
  schema names, exact receipt digests, unknown-major rejection, and tamper
  detection.

Mandatory focused commands are documented in `AGENTS.md`. Complete Windows acceptance additionally validates the visible bootstrapper, installed scope marker, exact running Manager root, packaged services, output integration, repair/update, and uninstall preservation.
