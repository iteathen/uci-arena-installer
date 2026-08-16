# UCI Arena Installer Hardening Architecture

Status: target architecture for the currently open Installer hardening/release issues  
Scope: Installer issues #6-#10, #15, #51, #76-#80  
Authority relationship: complements `specs/00_READ_FIRST.yaml`, `specs/suite_lego_boundaries_v1.yaml`, `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`, and the active installation contract. It does not make provider behavior Installer-owned.

## 1. Objective

The Installer must converge on one stable mechanism layer in which ordinary application changes replace artifacts, manifests, composition data, or release media rather than installer control flow.

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

Issue ownership: foundation of #7; reused by #8, #9, #10, and #80.

### 2.2 ExactDocumentReader

Consumes `FilesystemIdentity` and returns immutable admitted bytes plus bounded identity metadata. Hashing, signature verification, and parsing must consume those same bytes; callers do not reopen the pathname.

Issue ownership: #7.

### 2.3 DurableStatePublisher

One durable state-publication mechanism owns bounded serialization, random private staging, flush/durability policy, atomic activation, previous-state preservation, concurrent-writer serialization/version checks, and incomplete-staging cleanup.

It is a mechanism, not the semantic owner of receipts, transaction journals, layout descriptors, trust state, or preservation records.

Issue ownership: #7; reused by #9, #10, and #80.

### 2.4 InstallLayout

One semantic layout contract owns where an installation is allowed to live. Build/package/runtime adapters may translate it, but may not restate competing root formulas.

The source contract uses semantic anchors plus relative segments. At initial installation it is resolved into an `installed-layout` descriptor containing the actual absolute roots and layout version.

On Windows, stable Installer registration records only enough information to locate the installed-layout descriptor and scope. Updates, repair, migration, and uninstall consume the installed descriptor; they do not reconstruct current defaults from environment variables.

Issue ownership: #80.

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

Requalification, timing evidence, credentials, bot policy, book production, and provider-specific repair remain provider-owned. Installer does not implement them. A provider may later be requalified through its own Manager/provider workflow; that is not part of the Installer transaction unless a future generic contract explicitly says so.

Issue ownership: Installer side of #51.

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

A crash at any checkpoint must deterministically select rollback (before `commit_decided`) or completion (after `commit_decided`).

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

Issue ownership: #10. This resolves the apparent tension between #9 transaction reuse and the existing purge lifecycle contract.

## 5. Release architecture

### 5.1 Neutral component provenance

Release trust is independent of release transport.

One neutral component-provenance record binds:

- component/version/target;
- source repository/project identity and exact source commit;
- artifact identity, digest, size, manifest identity, and dependency declaration;
- producer type and producer identity;
- signed attestation/signature identity;
- replay/version information required by policy.

Producer adapters may include:

- GitHub Actions OIDC/Sigstore;
- explicitly configured local/offline signing authority.

GitHub workflow path, GitHub OIDC issuer, workflow trigger, prerelease tag, and GitHub Release location are adapter data, not universal provenance semantics. Local/offline official provenance is signed and fail-closed; unsigned-development mode is never promoted into official trust.

Issue ownership: #76.

### 5.2 ReleaseInputPlanner

One planner consumes:

```text
explicit previous qualified suite
+ explicit target suite version
+ verified changed component candidate(s)
+ target platform/trust policy
```

It produces one self-contained release-input directory containing generated composition, release lock/provenance bindings, release plan, exact assets, and trusted metadata.

For routine application releases, component dependency/role/installation-contract changes are not silently accepted. A candidate whose manifest changes architectural dependencies or installer protocol requirements is classified as `architecture_review_required` and stops the routine path.

The planner emits a semantic diff of changed and unchanged component identities. Operators/agents do not hand-copy full suite compositions for a one-component update.

Issue ownership: #78; depends on #76 for neutral provenance.

### 5.3 Stable Windows integration and Manager dispatch

The Windows MSI/bootstrap integration brick owns durable OS integration only. It does not own versioned Manager application bytes.

Stable Installer-owned dispatch entrypoints resolve the active `manager.core` receipt and declared Manager entrypoint at runtime:

```text
manager dispatch role = service | cli | desktop
 -> locate installed-layout descriptor
 -> load/validate active manager.core receipt
 -> resolve manifest-declared role entrypoint beneath active component root
 -> launch with bounded argument/environment contract
```

Windows service registration, Start Menu entry, and PATH exposure target stable Installer-owned dispatch entrypoints. Manager application internal paths remain manifest data.

Initial install ordering registers stable integration without requiring the Manager payload to be running, installs/activates `manager.core` through `arena-setup`, and only then starts/enables the Manager runtime as appropriate. MSI repair repairs the stable integration brick and must not downgrade an independently updated Manager payload.

Issue ownership: #77; depends on #80 and the transaction/lifecycle contracts.

### 5.4 Stable fresh-install bootstrap + versioned suite media

Fresh install uses the same logical suite-media/component contracts as package-only update.

The stable bootstrap/integration executable is independently versioned from the suite and changes only for bootstrap/integration/runtime-protocol changes. Ordinary component updates replace only suite media.

A single-file convenience artifact may append/package the stable bootstrap plus new suite media, but application-only releases must not compile a new MSI/Burn/bootstrapper/native Installer binary. Final per-release signing/data packaging is allowed; compilation is not required unless the stable integration protocol changed.

Issue ownership: #79; depends on #76, #77, #78, and #80.

## 6. Open-issue ownership map

| Issue | Architectural owner | Direct implementation scope | Explicitly out of scope |
|---|---|---|---|
| #6 | ServiceControl | neutral service port + Windows/Linux adapters + tests | transaction policy, provider lifecycle, Manager behavior |
| #7 | FilesystemIdentity / ExactDocumentReader / DurableStatePublisher | exact-byte admission and durable state mechanics | downloads, archives, install transaction policy, removal policy |
| #8 | ArtifactAcquirer / ArchiveExpander | bounded acquisition/cache/extraction using #7 primitives | TUF semantics, transaction commit, product-specific package behavior |
| #9 | InstallTransactionCoordinator | lease, journal, phases, effect/compensation orchestration, recovery | duplicate filesystem/service/provider implementations |
| #10 | RemovalLifecycle + ExactDeletion/PurgeExecution | reviewed uninstall/purge execution using #7/#9 primitives | new install transaction engine; purge rollback fiction |
| #15 | acceptance scenario only | Personal live-provider failure/rollback regression for #9/#10 | standalone rollback implementation |
| #51 | ProviderLifecycle disposition contract on Installer side | generic `ready/safe_inactive/failed` handling and evidence | Lichess/timing/evidence/provider-specific requalification logic |
| #76 | ReleaseProvenance | neutral provenance schema + GitHub/local producer adapters | suite planning, bootstrap, provider build internals |
| #77 | StableWindowsIntegration + ManagerDispatch | stable OS registration/dispatch and Manager payload separation | Manager control-plane behavior |
| #78 | ReleaseInputPlanner | deterministic routine release planning/input bundle | provenance cryptography (#76), transaction runtime (#9), bootstrap (#79) |
| #79 | StableFreshInstallBootstrap | stable bootstrap consuming versioned suite media | application/package internals, release planning rules |
| #80 | InstallLayout | one semantic layout + resolved installed-layout discovery/migration | transaction semantics, Manager application layout internals |

## 7. Dependency order

Foundation work can proceed in parallel where independent:

```text
#6 ServiceControl ----------------------+
#7 Filesystem/Exact State --+           |
                            +-> #8       |
                            +-> #80 -----+----> #9 Transaction
#51 ProviderLifecycle ------------------+          |
                                                  +-> #10 Removal
                                                  +-> #15 acceptance
                                                  +-> #77 Manager integration

#76 Provenance -> #78 Release planner -> #79 Fresh-install media
                       ^                    ^
                       |                    |
                      #77 ------------------+
                      #80 ------------------+
```

#77 may develop its stable dispatch contract after #80 is fixed and may test transaction integration against the #9 contract; final completion waits for the required lifecycle behavior. #79 is intentionally late because it consolidates the stable bricks rather than creating replacements for them.

## 8. Implementation slicing rule

These open issues are architectural parents. **Do not assign a parent issue directly to a lower-capability implementation agent.**

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
