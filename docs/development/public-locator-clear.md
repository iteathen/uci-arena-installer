# Public saved-locator reset

Status: source candidate with scoped native unit and public CLI conformance.

Add `arena-setup --json import --kind KIND --clear` as the explicit alternative
to `--path PATH`. Clear excludes managed-copy and authority-mode modifiers. The
same supported import kinds apply; Restaurant remains detect-only. No new
provider-specific branch or launch-argument interpretation belongs in Installer.

Revoke only the selected saved locator, then use the existing manifest-selected
consumer renderer and configuration verification/rollback lifecycle. Preserve
every imported asset, managed-copy payload/receipt, other locator, and credential
file. Repeated clear of an absent key succeeds after normal affected-consumer
rerender. Required consumer binding failure restores the prior locator and all
changed consumer documents. Reject linked/irregular locator boundaries before
mutation; report original and rollback failures separately.

TDD covers an actual saved UCI override with a retained managed launch-profile
binding, idempotence, unknown/detect-only kinds, CLI action/modifier conflicts,
asset/credential preservation, and a late consumer-write failure after earlier
documents changed. Run only an isolated two-job native unit/CLI build with the
existing source-matched dependency cache; root owns the eventual full suite
build. This is not a new crash-recovery or installed-layout-locator mechanism.

The public `arena_setup_import_result_v1` clear response has `mode` equal to
`clear_saved_locator`, `state` equal to `cleared`, `removed` indicating whether
the selected saved key existed, and the existing `refreshed_consumers` list.
It returns no referenced path or credential contents. Clearing does not uninstall
any component or remove a separately valid install-receipt selection; that is
the public component removal lifecycle. The current live engine was observed as
an old install-receipt selection, so the UCI override test is conditional seam
coverage, not evidence that a live saved engine key existed.

The managed-copy single-game profile manifest carries its profile entrypoint
and exact evidence files, but no discovery declaration or activated resource.
The native test retains that receipt and all profile files while proving that
the cleared profile is absent from both generated configuration and generated
context projection, even with the default receipt-fallback source policy.
Current Bot profile bindings additionally require `saved_locator` authority.

The native test also rejects a generated-config target that is a directory;
the existing publication routine would otherwise replace an empty directory.
That rejection restores earlier consumer writes and the removed locator. Clear
backups use the existing UUID generator, refuse a preexisting destination, and
include the retained backup location in rollback/cleanup failure diagnostics.
This preserves the distinction between a failed mutation and committed clear
whose disposable backup could not be cleaned up.

Compensation failures are independent facts, not a successful rollback. The
original forward failure stays first; each failed consumer restoration names
its target and recovery backup, retains recoverable prior bytes, and records
`rollback_failed` rather than `restored` in the existing retained-write ledger.
Backup and target boundaries validate before destructive recovery. Clear uses
that existing retained-write route to keep consumer backups until commit, then
cleans them separately; cleanup failure cannot restore the locator over already
committed consumer state. Historical path-import success cleanup is unchanged.

A deterministic Windows delete-sharing fixture denies replacement of the
original config while allowing backup reads. It proves failed compensation is
reported and exact prior bytes remain available; it does not claim that the
locked original target was lost. Locator and generated-projection restoration
are checked separately.
