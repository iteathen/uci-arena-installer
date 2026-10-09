# Suite release authority

This repository is the only UCI Arena suite-composition and installer release
authority. Product repositories publish immutable component candidates; this
repository binds compatible candidates to a reviewed product composition,
acquires and verifies their exact bytes, and builds the installable suite.

`catalog.lock.json` is deliberately checked in with `release_ready: false`
until every composition entry has reviewed Windows/Linux or portable candidate
rows with exact artifact, result, provenance, source, and dependency identity.
It is planning data, not installable media. Validation and dry-run inspection
may read it; mirroring, acquisition, assembly, signing, installation mutation,
and publication must reject it.

The lock records the exact composition filename and SHA-256. Component ids,
versions, repositories, and dependency edges come from that composition rather
than from hardcoded release-tool tables. Updating one provider changes its
candidate rows and composition data, not installer orchestration.

Provider candidates remain private and independently attested. The optional
Installer prerelease mirror is digest-pinned transport only: original provider
identity and Sigstore provenance remain authoritative. No private repository,
token file, or credential is made public or copied into release metadata.

## Lock v3 complete assembly

New release work uses `arena_suite_release_lock_v3`, an exact composition and
digest-pinned `arena_provenance_trust_policy_v1`. Legacy provider/managed signed
results remain original byte-identical evidence; the public common verifier
normalizes them, preserving every keyless signature/SCT/tlog/DSSE gate. Local
keyed producers remain behind the same public verifier and explicit public-key
policy. There is no repository-name verification dispatcher for v3.

Invocation data, not the lock, supplies a bounded JSON map of selected component
ids to absolute candidate directories. Every directory contains exactly its
three named original files: artifact, evidence and bundle. Reports, extracted
inspection copies and transport metadata belong outside those directories.

```text
node tools/release/release-lock.mjs <lock.json> <composition-directory> --validation-mode official --trust-policy <absolute-policy.json> --required-targets windows-x86_64
node tools/release/clean-build.mjs --complete-development --validate-inputs --platform windows-x86_64 --lock <lock.json> --composition-directory <directory> --validation-mode official --trust-policy <absolute-policy.json> --candidate-directories <absolute-map.json>
node tools/release/clean-build.mjs --complete-development --platform windows-x86_64 --lock <lock.json> --composition-directory <directory> --validation-mode official --trust-policy <absolute-policy.json> --candidate-directories <absolute-map.json> --output-root <fresh-output>
```

`--validate-inputs` performs common provenance verification and emits only
`arena_suite_inputs_verified_v1` on stdout; it neither extracts nor builds.
That record proves input provenance, not native manifest or installation
admission. The archive/inventory/dependency gates remain independently required.
Cosign is resolved through the public pinned resolver; `COSIGN_EXECUTABLE` can
select an already admitted 3.0.6 executable. No key or credential contents are
read by this routing layer.

Complete assembly stages exact input bytes in its owned transaction, verifies
all rows before any extraction, checks canonical manifest targets, exact manifest
digests/inventories and composition dependencies, then invokes the existing
Manager MSI/Burn builders. The catalog consumes normalized artifact facts.
Async verification/build failure cannot publish partial output or replace the
prior completed output. Cleanup checks resolved ownership/containment and rejects
reparse targets; old PID-named directories are not swept. If publication commits
but old-output cleanup fails, the error states that commit and retained location.

V3 complete output uses `arena_clean_build_result_v2` with normalized verified
component inputs and exact lock/policy/mode identity. Historical v2 complete
output retains `arena_clean_build_result_v1`. The existing routine update caller
joins the same awaited candidate lifetime; it is not a fresh Manager MSI/Burn
assembly substitute or an installed-layout authority bypass. Complete outputs
remain explicitly unsigned suite candidates pending their separate release and
installation gates; verified component inputs do not imply suite signing.

## Bounded Windows Manager MSI diagnostic

`tools/release/windows-manager-msi-cycle.ps1` exercises one already-built,
fixed-scope Manager MSI with a single MSI invocation (`msiexec`). `prepare` records
the exact candidate and quarantines no state; `install` preserves prior state
by same-volume rename; repeatable `smoke` runs inspect the same installation;
and `restore -ConfirmRestore` returns the preserved state without deleting the
test state. `CandidateSourceCommit` binds adopted media to its Installer source.

This diagnostic does not build packages and does not replace full-suite
acceptance. The final suite cycle must exercise the corresponding setup bundle,
component selection, providers, repair, upgrade, ordinary uninstall, preserved
state revalidation, and the separate explicit purge lifecycle.
