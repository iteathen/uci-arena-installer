# Current-generation Windows package recovery (SP-010 proposal)

Status: incomplete WIP, paused at the owner's request. This branch must not be
merged, released or used to install the suite. The production chain is unchanged
and still has the reproduced late-failure rollback defect.

Completed work: a uniquely owned real Windows MSI/Burn RED fixture reproduced
loss of changed Manager bytes and verified fixture cleanup; the initial dormant
native MSI package adapter built with arena-setup. A subsequent extraction of
the existing apply cleanup helper and unused proposed public declarations has
not been rebuilt or regression-tested. The adapter has not been invoked to
restore any package and has no production call site.

Missing work: exact journal/id/root admission, shared mutation exclusion,
pre-MSI media preparation/refusal, retained apply compensation, package-aware
release-media retention, callback and CLI handoff, common commit/recovery,
scope/service-state restoration, runtime capability/version 0.2.4, production
helper native qualification, regression and independent review. No production
success or exact restoration claim is supported by this WIP.

This is the bounded implementation proposal for the existing preservation
guarantee. It is not transaction-v2 completion or stable Manager dispatch.
The native changed-bytes RED fixture reproduces the current failure:
`node tools/release/windows-suite-recovery-fixture.mjs E:/uci-arena-task-builds`.

## Ownership and journal

SetupEngine remains the current-generation transaction owner. Extend its one
`arena_install_transaction_v1` journal with a closed `manager_msi` package effect
and retained compensation; do not create a second coordinator or per-file MSI
manifest. Windows Installer continues to own package files and registration.

The bundle creates a random canonical UUID recovery id in its BAFunctions
adapter before planning an install/repair/modify. It is not operator-overridable.
The pre-chain helper durably establishes one exact scope/root-bound package
journal. Every later helper invocation supplies the same id, bundle owner,
scope and root tuple and rejects mismatches. Package commands never infer these
facts from receipts or MSI registration after mutation.

Pending package recovery blocks another package transition and normal component
mutation through this transaction owner. Terminal package records remain audit
evidence; this work does not interpret historical v1 journals as v2 recovery.

## Exact command surface

All commands use existing fixed scope and explicit root arguments plus:

* `--package-transaction <UUID>` binds the package journal.
* `package-recovery prepare --integration-root <path>
  --target-msi <path> --target-msi-sha256 <digest>
  [--prior-msi <path> --prior-msi-sha256 <digest>]` prepares the MSI effect.
* Existing `upgrade`/`repair` with `--package-transaction` activates and verifies
  components while retaining compensation. Its result is `package_verified`,
  not `committed`.
* `package-recovery rollback --burn-rollback <0|1>` applies exact component
  compensation when Burn is rolling back. Normal user uninstall remains the
  existing uninstall lifecycle and never uses an old package recovery record.
* `package-recovery restore-msi --burn-rollback <0|1>` runs after Burn's Manager
  MSI rollback and restores the exact package selected by journal disposition.
* `package-recovery commit` makes the common durable commit decision and
  performs post-commit cleanup. It is the last chain package.

The BA's execution callback supplies `--burn-rollback` from its documented
`fExecute` value. A cached executor or command-line guess is not authority for
deciding rollback. Burn process cancellation waits for the helper's existing
bounded lifecycle rather than abandoning it midway through compensation.

## Before Manager replacement

The guard validates fixed scope, selected roots, opposite-scope exclusion,
target original MSI digest and its package identities. It determines the exact
installed Manager product through Windows Installer's public API. On upgrade,
the complete original prior MSI must have an independently established digest
pin, match that exact product/package/upgrade identity and scope, and remain
physically admitted. A package GUID alone is not provenance.

An explicit reviewed original MSI and digest can bootstrap a legacy installation.
Subsequent successful installations retain the original MSI and its package-level
identity/digest with release evidence. Missing, malformed, substituted or
unavailable prior media returns a bounded `manager_recovery_media_required`
problem before the Manager MSI runs. No dialog, cache-name search, download or
historical Burn execution is a fallback. Windows Installer's internal LocalPackage
is not treated as original source media.

Privately retain verified copies of both prior and target original MSIs before
mutation, binding their digests and product/package identities in the same
journal. Fresh install has explicit prior-absent evidence. The copies contain
only package media, never Manager data, credentials or provider workspaces.

## Component compensation and common handoff

Extract the existing apply compensation and cleanup into private helpers used
by immediate apply and package handoff; do not duplicate the rollback engine.
Package apply retains receipt/configuration/document backups, replaced roots,
staged replacement controllers and workspace/locator compensation until the
common decision. No superseded-root cleanup runs before that decision.

Existing feature removal must be inside the same pending compensation boundary
or refused before mutation when its existing removal transaction cannot be
retained. Do not silently commit removals through a separate transaction.
Provider control failures remain failures; no lifecycle error is waived.

Retained release-media publication, Manager state restoration and the bounded
forward result must be completed or explicitly accounted for before the common
decision. A result-file failure cannot change a committed transaction into an
uninstall-shaped compensation request.

On any pre-decision failure/cancellation, exact journal compensation restores
prior components and receipts; Burn rolls back the new Manager MSI; the guard
then reinstalls the privately retained prior original MSI with the frozen scope
and roots. Fresh install instead verifies that the target product is absent.
The original forward failure and each component/MSI restoration failure remain
separate. Failed restoration retains recovery inputs and reports
`rollback_failed`, never `restored`.

After the common decision, compensation is forbidden. If Burn cancellation or
late infrastructure failure uninstalls the decided target MSI, the final guard
reinstalls the privately retained target MSI. The journal remains committed with
explicit cleanup/recovery failure until the decided installation is verified.
Never describe a post-decision partial recovery as an exact previous rollback.

## Qualification and limits

The current RED fixture installs a 1.0.0 private MSI, verifies prior bytes,
successfully major-upgrades to 1.0.1 changed bytes, then fails a later EXE with
exit 17. It asserts prior payload and MSI-owned registration restoration and
always removes its unique test products/key. It uses no service, PATH or live
Manager identity.

Extend it for prior-media absence/substitution refusal before MSI mutation,
failed fresh install, deferred component success then injected later failure,
same-version repair, normal uninstall, rollback failure, repeated recovery,
scope/root/id mismatch and post-decision forward recovery. The actual fixture
must execute production helpers and generated package authoring; regex checks
alone do not qualify recovery. Machine service qualification belongs in an
isolated elevated environment.

Required new package commands are a runtime protocol capability. Source remains
0.2.3 until the exact final interface is reviewed; release assembly must require
the eventual new capability/version and cannot compose an older runtime merely
because its historical version string still matches.
