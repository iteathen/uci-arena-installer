# Installer architecture

The UCI Arena installer is a product-specific assembly of replaceable bricks. `arena-setup` is the native transaction engine; Windows bundles and Linux packages are launch and lifecycle adapters; the release catalog and component manifests contain package knowledge.

The stable dependency flow is:

```text
Launcher -> Orchestrator -> Configuration + Manifest
                         -> Environment + Installation inspection
                         -> Planner -> Staging -> Deployment
                         -> Validators -> Commit
                         -> Backup + Rollback on failure
                         -> Structured log and result
```

No brick may discover a neighboring implementation. Inputs are explicit, paths are absolute and normalized at boundaries, external commands use argument arrays, and secrets are redacted or represented only by external path references.

The scope-neutral Windows launcher is only a front door. It selects Personal or All users and delegates to the matching fixed-scope bundle. The fixed bundles retain separate product identities, cache identities, privilege models, install roots, data roots, PATH ownership, and Manager lifecycle. The launcher never becomes an installer engine.

Windows release assembly is Installer-owned. The Manager repository publishes
one immutable `manager.core` artifact with a root component manifest; it does
not publish or author the suite MSI or Burn bundle. The Manager MSI builder
validates every manifest-owned byte and applies the declarative
`manager-integration.json` policy for fixed roots, PATH, shortcut, scope marker,
and the machine-only service. The suite-bundle builder consumes that MSI, the
Installer-owned `arena-setup` runtime, and exact catalog media. Neither builder
accepts a Manager source or CMake install tree.

`diagnostic-review-inputs.mjs` is the private package-source adapter for the
pre-merge acceptance cycle. Its declarative source set names one exact private
prerelease and target commit for every composed component. It downloads exactly
three bytesets per component—artifact, release result, and Sigstore bundle—then
constructs a complete target-specific review lock. The review lock is never
release-ready and cannot enter provider mirroring, suite signing, or promotion.

`clean-build.mjs` is the release-assembly orchestrator. It validates the lock
and composition before compilation, verifies every original provider or
Installer-managed signature and independent DSSE predicate, extracts component
archives into isolated roots, validates their manifests and dependency truth,
builds the catalog, builds Installer-owned platform integration, and publishes
only a complete digest-bound output directory. Official mode accepts main-only
release-ready provenance. The explicit diagnostic-review mode changes only the
permitted exact workflow ref and emits `unsigned_suite_candidate`. Both modes
consume immutable artifacts and never read another repository's source tree.

Clean-build output is transactional: work and publish roots are fresh and
bounded; the last complete output is retained until all native tests, package
adapters, launcher envelopes, and artifact digests succeed. A failure removes
only the new transaction root. The result records the composition and lock
digests plus every component repository, version, target, source commit,
workflow ref, artifact digest, result digest, and provenance-bundle digest.

Linux release assembly uses the same verified component inputs, catalog
builder, Installer runtime, and transactional publication boundary as Windows.
The Linux package adapter validates immutable `manager.core` bytes with the
Manager component version, then applies the suite release version only to DEB,
RPM, and portable-tar package identity. The portable `.run` adapter receives
trust and transport modes independently: development media explicitly enables
the unsigned-development catalog, official media requires embedded TUF root
metadata, and only offline media passes `--offline`. Its temporary extraction
root is removed after both successful and failed setup execution.

The catalog derives dependencies from product composition and activation roles
from component manifests. Windows feature labels and selection belong only to
`packaging/windows/suite-bundle.json`; the standard opening-book artifact has a
separate default-on control from Book Forge. Online and offline child bundles
share exact accepted component bytes, while only offline apply and repair pass
`--offline` to `arena-setup`.

Package content, entrypoints, dependencies, health checks, preservation, and uninstall ownership belong to `arena_install_catalog_v1` and `arena_install_component_v1`. Policy belongs to the installation contract and versioned configuration. Mechanism belongs to installer components.

Large external chess assets deliberately use a different compatibility rule
from executable components. Discovery and preservation are version-permissive:
standalone books, partial Syzygy collections, and parseable timing evidence stay
visible even when optional metadata, suite versions, or runtime identity differ.
Consumers enforce only the capabilities needed for a requested activation and
must retain incompatible timing evidence as cataloged evidence with an explicit
migration or requalification reason.

Timing authority is semantic, not a build-file identity. The Engine-owned
contract binds the timing/search contract, model semantics, provider/runtime
and hardware identity, and effective search topology. A qualified executable
digest remains provenance; a timing-compatible rebuild is reported without
invalidating the curve. Exact executable identity remains mandatory for fresh
measurement and complete-game/single-game evidence that claims what actually
ran.

Release assembly never regenerates, promotes, or reselects timing evidence.
Installer, repository, branch, packaging, and provenance changes are not
timing-relevant inputs. A qualified timing profile remains authoritative unless
one of its declared semantic timing inputs changes, and the runtime consumer
must revalidate that semantic identity before use.

Indexed discovery is split at a platform port. Installer-owned policy supplies
typed resource kinds, exact extensions, and bounded filename globs; Windows
Search and locate adapters translate those rules and return path evidence only.
Content inspectors classify books, Syzygy collections, timing evidence, and
Lichess single-game/capacity/transport profiles without activating or copying
them. Within one source class, advisory content quality ranks more developed
assets before deterministic path order without relaxing later activation gates.
A saved locator is the operator-selected resource and outranks a bundled
bootstrap candidate, while the latter's receipt remains separate installation
authority. If both records name the exact same managed resource, the receipt is
selected so known installation authority is not projected as retained unmanaged
state. Inventory re-inspects saved knowledge and evidence content instead of
treating cached locator validation as current authority. If the operating-system
index is unavailable, an explicitly selected
root may be inspected only at the root and its direct children.

Removal is implemented as a separate `RemovalLifecycle` brick behind the
`SetupEngine` façade. Its contract separates `plan_uninstall` from `uninstall`
and `plan_purge` from `purge`. Provider startup behavior enters through explicit
hooks supplied by the composition façade; removal code does not discover or
interpret a provider implementation. The same brick is directly testable with
fake hooks and synthetic receipts.

The governing specification is `specs/installation_contract_v1.yaml`. The remaining files in this directory define the focused contracts agents must read before installer work.
