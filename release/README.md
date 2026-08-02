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
