# Installer test matrix

Automated tests use isolated temporary roots, synthetic manifests, fake packages, controlled state trees, and injectable platform adapters. A test asserts filesystem state, ownership, receipts, logs, backups, rollback, final classification, and exit code.

Required scenarios:

- clean and repeated clean install;
- spaces, Unicode, long paths, trailing separators, and unrelated working directory;
- valid upgrade, invalid downgrade, repair, reinstall, replacement, and uninstall;
- missing or corrupt payload and invalid manifest;
- missing runtime and failed dependency command;
- locked destination and partial existing installation;
- interrupted stage and staged validation failure;
- failure after backup and during replacement;
- rollback success and rollback failure;
- user-data preservation and application-file replacement;
- idempotent configuration, PATH, shortcuts, startup, and migration;
- post-install health success and failure;
- cleanup after success and failure;
- Personal delegation without elevation;
- All-users delegation to the elevation-requiring machine bundle;
- deterministic online and offline launcher envelopes, exact embedded bundle
  digests, corrupt-envelope rejection, and temporary-child cleanup;
- installed-scope maintenance and explicit cross-scope block;
- and one startup owner with no duplicate Manager, Book Forge, Evidence, or Lichess daemon.

Mandatory focused commands are documented in `AGENTS.md`. Complete Windows acceptance additionally validates the visible bootstrapper, installed scope marker, exact running Manager root, packaged services, output integration, repair/update, and uninstall preservation.
