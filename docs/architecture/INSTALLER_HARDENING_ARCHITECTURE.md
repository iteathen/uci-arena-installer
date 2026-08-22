# UCI Arena Installer Hardening Architecture

Status: target architecture for the currently open Installer hardening/release work  
Scope: Installer issues #6-#10, #15, #51, #76-#80, #161-#164 and delegated implementation packets derived from them  
Authority relationship: complements `specs/00_READ_FIRST.yaml`, `specs/suite_lego_boundaries_v1.yaml`, `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`, and the active installation contracts. It does not make provider behavior Installer-owned.

## 1. Objective

The Installer must converge on one stable mechanism layer in which ordinary application changes replace artifacts, manifests, composition data, provenance, or release media rather than Installer control flow.

The desired steady state is:

```text
verified component artifacts + manifests + provenance
                     |
                     v
            release-input planner
                     |
                     v
              versioned suite media
                     |
          +----------+----------+
          |                     |
          v                     v
stable fresh-install      installed update path
bootstrap/integration       stable arena-setup
          |                     |
          +----------+----------+
                     |
                     v
             transaction engine
                     |
          +----------+----------+
          |          |          |
      filesystem   service   provider lifecycle
       authority   control      adapter
```

Application repositories remain adjacent products. Installer consumes their immutable artifacts and contracts only. Installer never repairs provider internals, interprets chess/provider semantics, or reaches into provider source trees.

### 1.1 Strong immutability acceptance invariant

The architectural destination is not merely “data-driven enough to reduce rebuilds.” The destination is a **strongly durable Installer mechanism** whose code and stable platform-integration bytes change rarely relative to application releases.

For a routine application-only release that remains within existing public installation contracts:

```text
changed application bytes / manifest data
                  |
                  v
       verified component candidates
                  |
                  v
        deterministic release plan
                  |
                  v
           versioned suite media
                  |
                  v
       SAME installer mechanism bytes
                  |
       verify -> stage -> transact
                  |
                  v
          updated installation
```

A routine release may change component versions, exact bytes, internal component file layout, manifest-declared relative entrypoints, optional owner-declared artifacts/resources, and other data already expressible by the current contracts. Such a change may require composition or compatibility review. Review does **not** by itself justify an Installer code change.

The supported normal path must require:

- no C++ Installer control-flow edit merely because an application changed;
- no new product-name branch or product-specific validator in generic `arena-setup` / `SetupEngine` logic;
- no Windows MSI/Burn/bootstrap recompilation merely because Manager/provider application bytes changed;
- no Linux stable integration-package/bootstrap recompilation merely because Manager/provider application bytes changed;
- no hand-maintained second semantic root/layout map;
- no operator-maintained cross-file release bookkeeping as authority;
- no provider source/build-tree dependency.

A release/build script adjustment is allowed when release mechanism or tooling genuinely changes. It is exceptional maintenance, not the normal response to a new application version.

A real installation-protocol change, public contract-major change, trust-bootstrap change, stable OS-integration change, security fix, or new platform capability may require Installer code. `ReleaseInputPlanner` must identify that as architecture/integration review rather than silently treating application drift as permission to recouple the system.

Issue #161 is the final cross-platform acceptance gate for this invariant. Individual child issues are not sufficient if an old product-aware, raw-root/current-default, duplicate-layout, or per-application bootstrap/integration path remains supported and reachable.

### 1.2 Platform separation without semantic divergence

Windows and Linux may have different adapters for filesystem identity, path rules, service control, installed-layout discovery, package-manager integration, elevation, and bootstrap packaging. Those differences remain below shared Installer semantic boundaries.

They must share the same lifecycle meaning:

```text
immutable component identity
-> explicit composition/selection
-> exact installed-layout authority
-> bounded acquisition/staging
-> one transaction truth
-> manifest-declared lifecycle/resource behavior
-> receipt / rollback / recovery truth
```

Platform adapters translate that meaning. They do not become separate product-aware installers.

## 2. Shared foundational bricks

The open issues overlap around filesystem identity, paths, lifecycle, and release authority. They must reuse the following owners rather than create issue-local mechanisms.

### 2.1 FilesystemIdentity

One platform adapter family owns physical path admission and identity checks.

Responsibilities:

- absolute/normalized boundary admission;
- no-follow file/directory open;
- symlink/junction/reparse/mount rejection where the operation forbids them;
- stable file/directory identity capture;
- ancestor identity validation;
- bounded path handling;
- target-platform path comparison/collision semantics.

It does not parse TUF, download artifacts, decide installation layout, execute lifecycle policy, or delete arbitrary trees by itself.

Issue ownership: foundation of #7; reused by #8, #9, #10, #80, and #162.

### 2.2 ExactDocumentReader

Consumes `FilesystemIdentity` and returns immutable admitted bytes plus bounded identity metadata. Hashing, signature verification, and parsing must consume those same bytes; callers do not reopen the pathname.

Issue ownership: #7.

### 2.3 DurableStatePublisher

One durable state-publication mechanism owns bounded serialization, random private staging, flush/durability policy, atomic activation, previous-state preservation, concurrent-writer serialization/version checks, and incomplete-staging cleanup.

It is a mechanism, not the semantic owner of receipts, transaction journals, layout descriptors, trust state, or preservation records.

Issue ownership: #7; reused by #9, #10, #80, and #162.

### 2.4 InstallLayout

One semantic `InstallLayout` owner defines where an installation lives and how exact installed authority is retained across Installer generations. Platform-specific policy/anchor/locator adapters may differ, but may not restate competing root formulas.

The common invariant is:

```text
stable platform locator
        |
        v
exact installed-layout descriptor
        |
        v
exact versioned layout policy
        |
        v
actual installed roots
```

Initial installation resolves explicit platform anchors into an installed-layout descriptor containing the actual roots, scope, installation identity, generation, policy identity, and stable paths. The exact policy/descriptor becomes persisted installation authority.

Routine update, repair, migration, and uninstall consume that installed authority. They do not reconstruct an existing installation from current environment variables, executable adjacency, repository defaults, package assumptions, or raw root arguments.

Platform ownership:

- #80 owns the Windows policy/known-folder/registry-locator adapter family and current IL-01..IL-07 Windows implementation sequence;
- #162 owns the Linux policy/anchor/stable-locator adapter family and Linux migration to the same persisted-authority invariant.

Do not stretch Windows child packets into Linux conditionals and do not create a second Linux semantic layout owner. One semantic concept has thin platform adapters.

### 2.5 ServiceControl

One platform-neutral port owns service actions/results.

Boundary types:

```text
ServiceAction = start | stop | restart | status
ServiceOutcome = completed | failed
ServiceState = unknown | inactive | activating | active | deactivating | failed
ServiceResult = action + outcome + state + bounded platform diagnostic
```

Each mutating action has one overall deadline. Windows SCM and Linux/systemd details remain adapter-private. Raw OS codes may appear only as bounded diagnostics and never as caller-facing semantic state.

Issue ownership: #6.

### 2.6 ProviderLifecycle

Installer invokes provider-owned lifecycle capability only through a versioned typed contract. Installer branches only on generic disposition, never provider-specific reason semantics.

For replacement/enable operations the terminal disposition is:

```text
ready
safe_inactive
failed
```

`safe_inactive` is accepted only when the provider/component contract explicitly declares that disposition safe for the requested lifecycle operation. A bounded provider-owned reason identifier may be preserved for operators/Manager but remains opaque to Installer.

Requalification, timing evidence, credentials, bot policy, book production, and provider-specific repair remain provider-owned. Installer does not implement them.

Issue ownership: Installer side of #51.

### 2.7 Product-neutral setup facade

`SetupEngine` remains an application facade over existing Installer owners; this does **not** introduce a new LEGO or a second transaction owner.

Its normal planning/input boundary must become composition-driven and product-neutral. Current hard-coded product/resource vocabulary is transitional and must not become the durable contract.

Composition/feature selection is data:

```text
suite composition
    -> declared feature/component roots
    -> dependency closure
    -> setup intent
    -> install plan
```

External or managed resources are represented through bounded Installer-owned installation semantics rather than current product identities. Examples of semantic categories that may be frozen by the owning contract include external file/directory references, managed immutable files/file sets, opaque bounded documents/profiles, credential path references, and component/entrypoint references.

The exact resource vocabulary must remain small and tied to real Installer mechanics. It must not become an arbitrary script/plugin/validator registry. Product semantic qualification remains component/provider-owned through declared typed contracts.

Human-facing labels such as “Syzygy tablebases” or “Lichess token” may come from composition/component metadata. Labels never become mechanism keys.

Issue ownership: #164. Existing TX-07 issue #135 is architecturally blocked until its product-aware `preset`/book/tablebase planning inputs are replaced by the neutral setup-intent shape. Its acquisition/staging/transaction responsibilities remain #8/#9-owned.

## 3. Transaction architecture

Issue #9 owns one `InstallTransactionCoordinator`. No other issue may add a second install/update transaction engine.

The coordinator consumes typed effect adapters for filesystem activation, generated configuration/documents, receipts, provider lifecycle, service control, knowledge activation, and layout/trust state. Adapters report effects and compensation evidence; they do not claim transaction commit.

### 3.1 Mutation lease

Exactly one mutation lease exists per installation scope. Install, upgrade, repair, and transactional uninstall acquire it before mutation. Purge uses the same scope exclusion authority but has different irreversible semantics described below.

### 3.2 Durable phases

```text
planned
  -> prepared
  -> activating
  -> verifying
  -> commit_decided
  -> cleaning
  -> committed
```

Before `commit_decided`, every completed mutation must have a recorded compensation and recovery rolls the transaction back.

`commit_decided` is the durable point of no return. It may be written only after all required final component/configuration/startup/receipt state has been activated and verified. After it, recovery completes cleanup and final reporting; it does not attempt to restore the previous installation.

Rollback states are explicit:

```text
rolling_back -> rolled_back
             -> rollback_failed
```

The original failure and every rollback/cleanup failure remain separately visible.

### 3.3 Journal operations

Every external effect has a stable operation id and records:

- operation kind and owner;
- expected pre-state;
- prepared inputs/identities;
- before-effect checkpoint;
- observed effect/result;
- verification result;
- compensation identity/result when applicable.

A crash at any checkpoint must deterministically select rollback before `commit_decided` or forward completion after it.

### 3.4 Receipts and startup

Receipts may be transactionally activated before the commit decision when runtime verification needs the new authority, but they remain compensatable until `commit_decided`. A transaction is not described as committed merely because a new receipt is currently visible.

Provider replacement may end `ready` or contract-declared `safe_inactive`. `safe_inactive` can therefore be a successful verified transaction state without falsely claiming the provider is live.

## 4. Removal architecture

### 4.1 Uninstall

Uninstall is a compensatable transaction coordinated by the #9 transaction engine. `RemovalLifecycle` remains the removal-domain planner/facade and supplies reviewed operations; it does not invent a parallel journal/lease system.

### 4.2 Purge

Purge is intentionally irreversible and is **not rollback-capable** after a deletion succeeds. It reuses mutation exclusion, exact filesystem identity, durable publication/evidence primitives, and reviewed plans, but executes through a monotonic `PurgeExecution` record:

```text
planned -> executing -> completed | partial_failure
```

Each deletion is identity-bound and independently recorded. Recovery resumes/revalidates remaining operations; it never pretends deleted user/state bytes can be rolled back.

Issue ownership: #10.

## 5. Release and integration architecture

### 5.1 Neutral component provenance

Release trust is independent of release transport.

One neutral component-provenance record binds component/version/target, source identity/commit, artifact identity/digest/size/manifest/dependencies, producer identity, signed evidence, and replay/version information required by policy.

Producer adapters may include GitHub Actions OIDC/Sigstore and explicitly configured local/offline signing authority. GitHub workflow paths, OIDC issuer, release tags, and transport locations remain adapter data rather than universal semantics. Local official mode remains signed and fail-closed.

Issue ownership: #76.

### 5.2 ReleaseInputPlanner

One planner consumes:

```text
explicit previous qualified suite
+ explicit target suite version
+ verified changed component candidate(s)
+ target platform(s)
+ trust/release policy
```

It produces one self-contained release-input directory containing generated composition, semantic release plan/diff, release-lock/provenance bindings, exact assets, trusted metadata, and suite-media inputs.

Routine application changes already expressible by current contracts are data changes, not Installer control-flow changes. Representable dependency/role/ownership differences may trigger explicit review, but only a genuine installation-protocol/integration change may require Installer mechanism changes.

The planner consumes #164 composition/feature intent rather than product-specific setup booleans. Package-only update and fresh-install media consume the same release-input authority. Windows and Linux target rows use the same planner semantics.

Issue ownership: #78; depends on #76 and consumes the product-neutral selection boundary from #164.

### 5.3 Stable OS integration and shared Manager dispatch

Stable platform integration is independently versioned from `manager.core` application bytes.

The shared semantic `ManagerDispatch` contract is:

```text
role = service | cli | desktop
 -> load exact InstalledLayout authority
 -> admit/validate active manager.core receipt
 -> resolve manifest-declared role entrypoint beneath active Manager root
 -> launch using a bounded platform adapter contract
```

The semantic contract contains no Windows registry, MSI, systemd, DEB/RPM, executable filename, or current Manager internal path identity.

#### Windows

`StableWindowsIntegration` owns Add/Remove Programs/MSI ownership, stable service registration, Start Menu, PATH, setup/update launch authority, and Windows dispatch adapter. It does not embed the versioned Manager application payload. MSI repair repairs integration only and must not silently downgrade an independently updated Manager.

Issue ownership: #77; consumes #80/#9/#6.

#### Linux

`StableLinuxIntegration` owns stable package-manager integration, systemd registration, desktop integration, CLI/PATH exposure, setup/update launch authority, and the Linux adapter for the same shared ManagerDispatch semantics. DEB/RPM integration must not embed/version-couple ordinary Manager application payload bytes in the steady state.

Issue ownership: #163; consumes #162/#9/#6 and the shared dispatch semantics frozen with #77.

### 5.4 Stable fresh-install bootstrap + versioned suite media

Fresh install uses the same logical release/setup/component contracts as package-only update on both supported operating systems.

The stable bootstrap/integration responsibility is independently versioned from the suite and changes only for bootstrap/integration/runtime-protocol changes. It owns scope/privilege handoff, opening/verifying suite media, stable lifecycle handoff, bounded UI/error presentation, and compatibility checks. It contains no current suite component file list or product feature algorithm.

The versioned suite-media responsibility is generated from #78/#76/#164 and owns composition/catalog, exact component packages, release plan/provenance/trusted metadata, media manifest/digests, and schema-owned component/feature selection.

Windows may use a native bootstrap/integration adapter. Linux may use a portable launcher/package adapter. Application-only releases replace media; they do not require compiling stable Installer/bootstrap/integration bytes.

A single-file convenience distribution may package already-built stable bootstrap/integration bytes plus new suite media deterministically. The packaging wrapper is transport, not a second application packaging truth.

Issue ownership: #79; consumes #76/#78/#77/#80/#162/#163/#164 and #9.

## 6. Open-issue ownership map

| Issue | Architectural owner | Direct implementation scope | Explicitly out of scope |
|---|---|---|---|
| #6 | ServiceControl | neutral service port + Windows/Linux adapters + tests | transaction policy, provider lifecycle, Manager behavior |
| #7 | FilesystemIdentity / ExactDocumentReader / DurableStatePublisher | exact-byte admission and durable state mechanics | downloads, archives, install transaction policy, removal policy |
| #8 | ArtifactAcquirer / ArchiveExpander | bounded acquisition/cache/extraction using #7 primitives | TUF semantics, transaction commit, product-specific package behavior |
| #9 | InstallTransactionCoordinator | lease, journal, phases, effect/compensation orchestration, recovery | duplicate filesystem/service/provider implementations |
| #10 | RemovalLifecycle + ExactDeletion/PurgeExecution | reviewed uninstall/purge execution using #7/#9 primitives | new install transaction engine; purge rollback fiction |
| #15 | acceptance scenario only | Personal live-provider failure/rollback regression for #9/#10 | standalone rollback implementation |
| #51 | ProviderLifecycle disposition contract on Installer side | generic `ready/safe_inactive/failed` handling and evidence | provider-specific qualification/remediation logic |
| #76 | ReleaseProvenance | neutral provenance + producer adapters | suite planning, bootstrap, provider build internals |
| #77 | StableWindowsIntegration + shared ManagerDispatch semantics | stable Windows OS registration/dispatch, Manager payload separation | Manager product behavior; Linux package implementation |
| #78 | ReleaseInputPlanner | deterministic cross-platform routine release planning/input bundle | provenance crypto, transaction runtime, bootstrap mechanism |
| #79 | StableFreshInstallBootstrap | stable platform bootstrap consuming versioned suite media | application internals, release planning rules |
| #80 | InstallLayout Windows adapters/policy | Windows exact installed-layout discovery/migration | Linux layout implementation; transaction semantics |
| #161 | cross-platform acceptance | strong immutability acceptance matrix only | implementation mechanisms |
| #162 | InstallLayout Linux adapters/policy | Linux exact installed-layout discovery/migration | Windows registry/layout redesign; Manager application packaging |
| #163 | StableLinuxIntegration + shared ManagerDispatch adapter | stable DEB/RPM/systemd/desktop/CLI integration, Manager payload separation | Manager product behavior; new dispatch semantics |
| #164 | product-neutral SetupEngine planning/resource boundary | composition-driven setup intent and neutral resource binding | product semantic algorithms; arbitrary plugins; transaction ownership |

## 7. Dependency order

Foundation work can proceed in parallel where independent:

```text
#6 ServiceControl ----------------------+-----------------------------+
#7 Exact filesystem/state --+           |                             |
                            +-> #8       |                             |
                            +-> #80 Windows layout ----+               |
                            +-> #162 Linux layout -----+----> #9 ------+----> #77 Windows integration
#51 ProviderLifecycle ------------------+             |               +----> #163 Linux integration
                                                      |               |
#76 Provenance -> #78 Release planner ----------------+               |
                    ^                                                 |
                    |                                                 |
#164 neutral setup --+-------------------------> #135 TX-07 integration|
                                                                      |
#77 + #80 + #162 + #163 + #164 + #78 + #9 -----------------------> #79
                                                                      |
all required owners ------------------------------------------------> #161
```

#80 remains Windows-scoped; #162 is its Linux companion. #77 owns the shared semantic ManagerDispatch contract plus Windows integration; #163 consumes the same semantics for Linux. #135 must not proceed with its current product-aware planning signature; #164 must be frozen and the packet/spec reconciled first.

#79 is intentionally late because it consolidates stable bricks rather than creating replacements for them. #161 is acceptance only and must not accumulate implementation code.

## 8. Implementation slicing rule

Architectural parent issues are **not** assigned directly to a lower-capability implementation agent.

Before implementation of a slice, the organizing model must provide:

- exact owning brick and parent issue;
- prerequisite issue/contract revisions;
- exact allowed files/directories or interface surface;
- fixed input/output types and invariants;
- forbidden adjacent systems and forbidden new authorities;
- required focused tests and failure injections;
- stop conditions when the task encounters an architectural ambiguity;
- expected report: changed files, tests/evidence, blocked observations.

The implementation agent does not choose follow-on architecture, broaden scope, modify adjacent product repositories, close the parent issue, or define `next_step`. Integration/reassessment and the next work slice remain organizing-model responsibilities.

A task that cannot be specified this way is not decomposed enough for delegated implementation.

## 9. Final completion rule

The hardening program is complete only when the supported normal lifecycle demonstrates the #161 matrix on Windows and Linux.

In particular, implementation is **not complete** when a new owner exists but the old authority remains reachable. Before closing the relevant parent, remove or make unreachable from supported normal operation:

- product-name-based setup/import control flow replaced by #164;
- raw-root/current-default reconstruction for installed Windows/Linux authority replaced by #80/#162;
- duplicate packaging root maps;
- Manager-application-coupled stable Windows/Linux OS integration replaced by #77/#163;
- per-application bootstrap/integration recompilation replaced by #79;
- manual cross-file release authority replaced by #78;
- ad-hoc transaction/acquisition/service/provider mechanisms replaced by their fixed owners.

Compatibility readers may remain only when explicitly bounded, read-only, and unable to become new-write or normal-path authority.
