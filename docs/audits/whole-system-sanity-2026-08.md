# Whole-system sanity audit — August 2026

Status: in progress; no soundness conclusion has been formed  
Scope: `UCI_Arena`, `uci-arena-manager`, `uci-arena-lichess-bot`, `uci-arena-book-forge`, `uci-arena-evidence-service`, and `uci-arena-installer`  
Method authority: the operator-supplied Comprehensive Sanity-Check Question List  
Architecture authority: `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md`

## Purpose and evidence rule

This is a whole-product audit, not a compile review. Every applicable audit
question will be assigned exactly one of `answered`, `contradiction`, `unknown`,
or `not_applicable`, with a justification and concrete evidence. An `unknown`
is not a pass. A material contradiction prevents an unconditional soundness
finding even when all current tests pass.

Expected answers were frozen below before implementation inspection. Existing
code and tests may supply or contradict evidence, but they may not silently
redefine these expectations.

## Coverage rule

The line review covers every tracked first-party source, public contract,
specification, schema, test, migration, build/release definition, package
manifest, lifecycle script, and user/operator document in all six repositories.
Generated files, vendored dependencies, binary artifacts, lockfile payload data,
and retained runtime data are not treated as first-party source lines. They are
covered by their appropriate evidence instead: reproducibility, schema,
inventory, digest, provenance, package inspection, and runtime validation.

The final file inventory records each tracked path as one of:

- `line_reviewed`;
- `artifact_validated`;
- `generated_from_reviewed_source`;
- `third_party_or_lock_data`;
- `operator_or_runtime_state`; or
- `unknown`.

No path may disappear from coverage merely because it looks unimportant.

## Frozen expected answers

These are expectations, not findings.

| Area | Expected answer before implementation inspection |
|---|---|
| Larger problem | The product must provide a comprehensible, recoverable, easy-to-set-up chess suite whose installed behavior matches its public promises across use, failure, repair, upgrade, reinstall, uninstall, and explicit purge. |
| Product completeness | An ordinary installed user can discover or add an engine, run complete local competitions, observe truthful progress, configure optional providers, integrate their published outputs, and diagnose failures without source checkouts or routine terminal work. |
| Repository ownership | Six repositories own six independently released products. Installer alone owns suite composition and deployment; Manager owns the installed control plane; providers own their domains; Engine owns chess/search/evaluation/inference and engine runtime tools. |
| Boundary shape | Repositories exchange immutable artifacts and versioned typed contracts. They do not import source trees, inspect private stores, pass arbitrary shell commands, or leak third-party SDK types. |
| Composition | New providers, search cores, model adapters, inference backends, knowledge adapters, validators, and installable components enter through implementation plus registration or declarative composition, without edits to unrelated existing implementations. |
| Authority | Validated receipts plus active roots and registrations establish installation authority. Live typed observations establish runtime authority. Retained files, stale status, directory presence, filenames, and desired state are not authority. |
| Manager | One unprivileged Manager service is the sole control-plane state authority. CLI, React dashboard, and native host use the same commands/projections. The broker accepts only typed allowlisted privileged operations. |
| Desktop experience | The thin native host starts without a console, obtains an authenticated local session, embeds the packaged React view, owns native dialogs and narrow typed setup handoff, and never becomes a second runtime authority. |
| Engine discovery | The verified `uci_arena.engine` component is recommended by component identity, while every conforming UCI executable remains a peer. Auto-detection is bounded, never model-name driven, never registers automatically, and manual add/probe is complete and easy. |
| Tournament system | Match, round-robin, gauntlet, Swiss, and SPRT planning, persistence, start/pause/resume/cancel, canonical games, restart recovery, complete disposition accounting, progress, history, and reports work end to end. React never fabricates lifecycle or progress. |
| Lichess Bot | The standalone provider works with any conforming UCI process, owns all Lichess policy/state/streams/scheduling, uses one process per admitted game, exposes typed Manager operations, and is fully testable against a fabricated loopback API. |
| Credentials | The token is an external operator-owned path. Only the Lichess provider adapter may read its contents. Manager, Installer, tests, release tooling, receipts, logs, argv, and persisted public state never read or expose it. |
| Book Forge | The standalone provider durably acquires, ingests, qualifies, compiles, validates, and atomically publishes immutable book snapshots. Manager consumes only its typed public projections; Engine consumes only published book artifacts. |
| Evidence Service | The standalone provider schedules exact Engine-owned evidence-runtime tools, owns durable job/dataset/profile publication and resource admission, and does not duplicate chess, UCI, referee, search, or statistical authority. |
| Provider status | Installation, configuration, startup registration, desired state, live activity, health, compatibility, and output availability remain distinct. Every service has visible active/inactive/blocked evidence and actionable remediation. |
| Provider outputs | Manager discovers service APIs only from verified component manifests, validates envelopes/identity/path/digest, retains bounded projections, and never opens provider databases or infers artifacts from filenames. |
| Chess core | One Engine-owned rules authority defines positions, legal moves, make/unmake, repetition, draw, and terminal truth. Search, UCI, Manager games, and evidence tools do not invent competing truth. |
| Search architecture | `minimax` and `mcgs` are independent `SearchCore` implementations selected once through composition. Shared contracts contain no private concepts; neither implementation contains branches for the other. |
| Persistent MCGS | During an active game, search continues until terminal game/tablebase truth or explicit lifecycle cancellation. Confirmed position changes reroot or generation-invalidate incompatible work; stale work cannot publish. Turn sequence does not own search lifetime. |
| Time management | The formulated timing-evidence-derived curve is model/provider/hardware identity-bound, loaded through a declared profile contract, fixed within a game/child generation, and updated by qualified offline evidence between games rather than silently mutating during one game. Discovery remains permissive even when evidence cannot be activated. |
| Evaluation | Generic evaluation contracts contain only Engine-owned types. Admission, priorities, batching, tickets, cancellation, generation invalidation, cache, backpressure, and telemetry remain provider-neutral. |
| Model semantics | Each model adapter owns features, orientation, repetition semantics, tensor contract, policy mapping, value perspective, and semantic identity. Model names do not label generic infrastructure or select unrelated control flow. |
| Inference | Search, evaluation coordination, and model-semantic adapters depend on an Engine-owned inference port. ONNX Runtime types and behavior are confined to the ONNX adapter; substitution tests prove fake backend and fake model interoperability. |
| Knowledge | Polyglot and Syzygy/Fathom are neutral knowledge adapters. Search consumes typed results and does not parse their external formats or call their libraries directly. |
| Large-asset discovery | Books, partial WDL/DTZ sets, and parseable timing evidence are found through bounded OS indexes, saved locators, receipts, manifests, PATH/standard roots, and explicit shallow roots. No whole-volume recursion occurs. Discovery is permissive and non-mutating. |
| Large-asset compatibility | Suite versions never gate discovery or preservation. Optional metadata, sidecars, partial coverage, legacy schema, and different runtime identity reduce capabilities or require migration/requalification only for the requested activation; unrelated operation remains available. |
| Installer architecture | Launcher, orchestration, manifests/config/policy, inspection, planning, staging, deployment, dependency adapters, migration, validation, preservation, backup, rollback, logging, reporting, and removal have coherent ownership and explicit structured contracts. |
| Install lifecycle | Clean install, upgrade, repair, reinstall, downgrade, replacement, uninstall, and purge are explicit plans. Mutation is plan-first, staged, validated, journaled, backed up where required, transactional where possible, idempotent, and recoverable. |
| Windows scope | Personal and All-users products are fixed-scope packages behind a thin neutral launcher. Personal never elevates; All-users truthfully uses UAC. Scope identities, roots, PATH, Manager lifecycle, and maintenance remain separate, and two authorities cannot coexist. |
| Suite packaging | A clean release contains functioning Windows and Linux suite installers and every selected immutable component, including Manager, Engine, Lichess Bot, Book Forge, Evidence Service/runtime, required runtimes, and declared default knowledge. Compilation alone is not lifecycle proof. |
| Normal uninstall | Normal uninstall removes executable/startup/integration/receipt authority and preserves valuable Manager continuity, books, Syzygy, profiles, locators, provider workspaces/outputs, retained media/trust/cache/log/transaction evidence, modified managed files, and every external file. |
| Purge | Purge is separate, previewed, explicitly confirmed, path-bounded, ownership-aware, symlink/reparse safe, incapable of deleting external or unknown-owned files, and exact-identity aware for modified managed files. Partial failure is explicit. |
| Reuse after reinstall | Preserved state is revalidated before use and cannot itself reactivate or impersonate an uninstalled component. |
| Security | Every untrusted path is normalized, root-bounded, and protected from symlink/junction/reparse escape. Artifacts have independent digest and provenance verification. Secrets are redacted without making errors unactionable. Privilege matches scope. |
| Concurrency | One installer per scope, one Manager authority, and one provider daemon per workspace. Cancellation, drain, reroot, publication, replacement, startup, receipt commit, and rollback ordering prevent stale or duplicate authority. |
| Performance | Interfaces are coarse at hot boundaries; node/tensor/record loops avoid unnecessary dispatch, allocation, copying, synchronization, logging, and validation. Background providers obey bounded resource leases and promptly yield to foreground work. |
| Operator trust | Every success corresponds to durable backend or provider authority. Errors identify owner, expectation, observation, rollback state, evidence location, and next action. Missing controls and prerequisite references navigate to the owning surface. |
| Falsification | Packaged execution, boundary traces, synthetic failures, substitution fixtures, and clean install/repair/upgrade/uninstall cycles must be capable of disproving the source-level theory. Mocks alone cannot prove the complete product. |

### Frozen Lichess Bot seam expectations

These provider-specific expectations were recorded after reading the governing
specifications and before inspecting the provider implementation.

| Seam | Expected answer before implementation inspection |
|---|---|
| Credential gate | Probe, live run, and startup installation validate every non-secret prerequisite before the transport adapter reads an external token file; status, configuration, account controls, startup status, and uninstall never read it. |
| API safety | One provider-owned coordinator serializes finite exchanges and stream handshakes, persists every 429 cooldown before propagation, blocks every request class for the full cooldown, and uses a generation/usefulness check immediately before delayed writes. |
| Streams and games | Exactly one global event stream and at most one stream per admitted game exist. Each admitted game owns one qualified UCI child; authoritative streamed state and independent legality gate every move submission. |
| Capacity | Preparing, pending, active, cancelling, and draining work remain capacity-visible. Cancellation and terminal races release one exact slot generation only after its child is closed; restart reconciliation precedes new admission. |
| Engine substitution | Any conforming UCI executable can be selected by configuration. No Engine source, model-name branch, repository-relative default, or provider-specific child behavior is required. |
| Timing | Qualified timing/capacity/curve evidence is identity-bound and fixed for one child generation. Missing timing evidence preserves conservative foreground operation but disables opponent-turn work; explicit invalid live evidence fails closed before credentials. |
| Persistence | One WAL-mode schema-v3 store owns scheduler, reservations, opponents, games, API safety, and leases. Replay is idempotent, runtime provenance is immutable after first binding, and legacy state is never upgraded by invented identity. |
| Manager boundary | Every exposed operation is fixed, typed, bounded, credential-free unless explicitly defined as a credential-gated probe/start/run, and declared in the provider-owned operation catalog and closed schemas. Manager never reads private configuration or SQLite. |
| Status and startup | Startup registration, desired state, live process identity, stream health, readiness, slot occupancy, and blockers are distinct. Stale runtime files never imply activity, and one provider daemon owns one workspace. |
| Release artifact | The component archive contains only the declared runtime closure and provider-owned contracts, carries exact inventory/digests/provenance, contains no test simulator or credential material, and is independently consumable by Installer. |

## Required question ledger

The final ledger follows all 37 operator-supplied sections in order. Each
individual applicable question receives its status, evidence references, and
justification. `Final Big-Picture Assessment` and `Final Technical Assessment`
remain intentionally blank until every preceding section is classified.

## Initial contradiction register

This register began with specification evidence and now also includes the first
broad implementation and test inventory. Detailed line review remains in
progress.

| ID | Status | Contradiction | Evidence | Consequence |
|---|---|---|---|---|
| SPEC-OWN-001 | corrected | Manager carried suite release locks, acquisition/assembly tools, a copied installation engine, CPack platform packages, lifecycle runners, protected-runner provisioning, and suite signing. Manager now retains only its manual `manager.core` candidate workflow and provider-owned provenance. Installer owns composition, package policies/builders, signing, protected-runner provisioning, and package-cycle evidence. | Manager commits `4d02e8b` and `a8d3a48` plus the current `agent/release-assembly` change; Installer commit `fd04e74` plus the current `agent/release-assembly` change; both repositories' `tools/release`, workflows, release metadata, and packaging sources. | Source-tree acquisition, platform packaging, and installation mutation no longer have competing repository authorities. Final package execution remains unproven under `PKG-E2E-011`. |
| SPEC-PKG-002 | corrected | Manager previously described and built platform installers while Installer described a different complete-suite artifact set. Manager now publishes only one manifest-bound `manager.core` archive/result per target. Installer validates that component plus its own complete runtime manifest, applies declarative fixed-scope Windows or Linux integration, and alone names suite media. | Manager `specs/installation_contract_v1.yaml`, `CMakeLists.txt`, and `tools/release/build-manager-component.mjs`; Installer `packaging/windows/manager-integration.json`, `packaging/linux/manager-integration.json`, `tools/release/build-manager-msi.mjs`, and `tools/release/build-manager-linux-package.mjs`; focused release tests. | Normal component changes no longer require package control-flow changes in Manager. Actual DEB/RPM and complete suite execution remain final-gate evidence rather than being inferred from source generation. |
| SPEC-PROVIDER-003 | corrected | Manager's obsolete release lock attributed `lichess.adapter` to `iteathen/UCI_Arena`. The lock and its hardcoded validator were removed from Manager; the Installer-owned composition and draft lock assign it to `iteathen/uci-arena-lichess-bot`, and the new lock validator derives repository/version/dependencies from composition. | Manager commit `4d02e8b`; Installer commit `fd04e74`; Installer `compositions/complete-arena-0.3.8.json` and `release/catalog.lock.json`; 24/24 Installer release tests. | This stale provider-authority path is closed. Real candidate rows are still absent because the checked-in lock is deliberately non-release-ready. |
| SPEC-KNOW-004 | corrected | The Engine knowledge-source specification named `uci_arena_manager_installer` as the discovery owner even though Manager and Installer are independent repositories with distinct responsibilities. The Engine spec now assigns acquisition to `uci_arena_installer` and names Manager's typed discovery/import boundary. | Engine commits `095714f` and `cc110cf`; `specs/uci_knowledge_sources_v1.yaml`; Installer architecture sections 2.2 and 2.6. | The specification-level ownership contradiction is removed; implementation conformance remains subject to the Installer/Manager review. |
| TEST-GAP-005 | unknown | Fast suites pass, but Book Forge skipped its real Stockfish/native guard/native launcher paths, Evidence Service skipped the pinned evidence-runtime match path, and the current Engine CTest graph exposes only two tests. | Local `npm run check` results on 2026-08-01: Book Forge 41 pass/5 skip, Evidence 28 pass/1 skip; Engine `ctest --test-dir build -C Release`: 2/2. | Unit/contract success is not yet evidence that packaged providers or Engine features are fully functional. |
| BUILD-HYGIENE-006 | corrected | The Engine build tree contained 38 obsolete `flashfork_*` executables. A dry-run-bounded `git clean -fdX -- build` removed the ignored tree; an empty-tree configure/build now emits only `uci_arena_engine.exe` and `uci_arena_knowledge_sources_probe.exe`. The clean run also exposed that the registered knowledge CTest executable was excluded from the default build; commit `cc110cf` makes registered CTest binaries a default-build dependency when `BUILD_TESTING` is enabled. | Empty-tree configure/build and CTest trace on 2026-08-01; Engine `CMakeLists.txt`; Engine commit `cc110cf`. | Stale naming can no longer contaminate current build evidence, and a normal clean build followed by CTest passes 2/2 without prior artifacts. Broader Engine test coverage remains unknown under `TEST-GAP-005`. |
| SPEC-OWN-007 | corrected | Manager compiled and packaged a divergent copy of Installer's `SetupEngine`, exposed one Manager-owned token-locator mutation endpoint, and shipped `arena-setup`. Manager commit `a8d3a48` removes the copied engine, executable, credential-recovery implementation, API mutation, and duplicated tests. Both Bots and Configuration now invoke the same narrow typed plan/apply handoff to the adjacent Installer executable. | Manager commit `a8d3a48`; 52/52 native tests; current Manager release/UI checks; staged Windows component containing the four Manager entry points and no `arena-setup`. Runtime-closure acceptance is tracked separately by `PKG-RUNTIME-010`. | Installation planning, locator persistence, consumer regeneration, validation, and commit have one source owner. Manager retains only redacted locator consumption and operator projection; platform package ownership is corrected under `SPEC-PKG-002`. |
| SPEC-CRED-008 | corrected | Bots and Configuration previously offered apparently equivalent token-file controls with different effects: Bots wrote only a locator through Manager, while Configuration used Installer's transaction and regenerated dependent provider configuration. The Manager mutation route and client type were removed; both surfaces now use Installer plan/confirm/apply without reading token contents. | Manager commit `a8d3a48`; `web/src/components/LocalSetupImportButton.tsx`; `web/src/api/client.test.ts`; `specs/bot_onboarding_v1.yaml`; `specs/openapi.yaml`. | The easiest token-locator workflow now uses the only lifecycle authority and cannot silently leave Book Forge or Lichess consumer configuration stale. Real credential-gated provider behavior is still unknown under `TEST-GAP-005`. |
| BUILD-CACHE-009 | corrected | Installer's existing CMake cache resolved vcpkg dependencies from the Manager build tree, making an apparently local Installer build depend on a peer repository's generated state. The stale cache was discarded and the pinned vcpkg checkout/cache now lives beneath Installer's own ignored `build/tooling` root. | Former Installer `build/windows-release/CMakeCache.txt`; pinned Installer `build/tooling/vcpkg` at baseline `cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`; current independent configure inputs. | Cross-repository generated-state coupling is removed. The final clean runtime build is deliberately deferred and remains required by `PKG-E2E-011`. |
| PKG-RUNTIME-010 | unknown | Synthetic MSI fixtures prove that the complete manifest-bound Installer runtime is overlaid and that mismatched Manager/Installer path collisions fail. A real Manager install-tree check then exposed unwanted Windows UCRT/API-set capture; the Manager CMake contract now excludes OS-owned UCRT payloads, but that correction has not yet been rebuilt. | Installer runtime/MSI tests; Manager 52/52 native tests before the final UCRT exclusion; real 155-file pre-correction install-tree trace; Manager `CMakeLists.txt` and component test. | No real package claim passes until the final clean component/runtime build proves the exact adjacent DLL closure and absence of OS-owned or unrelated binaries. |
| PKG-E2E-011 | unknown | Installer-owned WiX source tests compile both fixed scopes, and Linux policy tests stage deterministic payloads with non-destructive maintainer scripts. No real DEB, RPM, online/offline setup bundle, install, repair, upgrade, uninstall, purge, or provider-complete packaged cycle has run against the corrected sources. | Installer release-tool tests and policies; operator instruction to defer installer builds until other corrections converge. | Packaging architecture may be locally coherent while real media still fails. Windows/Linux installer builds and one maximally reused installed-system cycle remain the final integrated gate. |
| LICHESS-UCI-012 | corrected | The Lichess provider treated UCI Arena Engine extensions, model identity, ONNX provider state, MCGS telemetry, timing profiles, and capacity evidence as requirements of the generic UCI process boundary. The provider now selects either `standard_uci_v1` or `uci_arena_extended_v1`; a minimal conforming UCI child is qualified by standard handshake, advertised options, and an independently legal fresh-root move without importing UCI Arena-only semantics. | Lichess commit `0f945b6`, draft PR #3; `lichess-adapter.mjs`, `smoke-uci-startup.mjs`, provider/service manifests, v7 configuration/specification; standard-UCI process and game-submission tests. | A conforming replacement engine no longer requires changes to Lichess streams, challenge policy, persistence, slot admission, legality, or move submission. Manager-side easy engine selection and installed-package execution remain whole-product evidence rather than provider-local claims. |
| LICHESS-CRED-013 | corrected | The authenticated account probe loaded live engine configuration and could fail on model/profile/engine prerequisites before reaching the provider-owned account boundary. It also attached the private stdin shutdown listener outside live run, keeping a successful headless probe process alive. Probe now loads only bounded provider policy, requires only an external token-file path, acquires the authority lease before credential access, contacts the account endpoint through the provider transport, and exits without installing a shutdown listener. | Lichess commit `0f945b6`; real loopback top-level probe tests with blank engine paths; recursive credential-field/bearer rejection and bounded regular token-file tests; 148/148 executed tests passed. | Account onboarding can be diagnosed independently of engine readiness while the token remains outside repository, package, public state, and non-credential operations. Real authenticated acceptance is intentionally deferred and cannot be inferred from the fabricated API. |
| LICHESS-LIFECYCLE-014 | corrected | Concurrent challenge/game observations and stale slot releases could race preparation, create duplicate game starts, or mutate a reused slot generation. Start operations now coalesce by game id; outbound `gameStart` can adopt the preparing child; preparing capacity remains discoverable; transitions and releases require the captured generation; reservations consume transactionally; and stale API safety writes cannot shorten a newer cooldown. | Lichess commit `0f945b6`; duplicate-start, outbound-response reordering, generation-safe release, concurrent admission, draining, recovery, cooldown durability, and lease tests. | Provider-local capacity and API authority now survive the tested reorderings. Packaged multi-process and real-network behavior remain falsification gates. |
| LICHESS-CONTRACT-015 | corrected | The packaged Windows startup path referenced a source-build directory, provider status could treat stale runtime metadata as live, package smoke assumed the private `runtime_status` command, and the Manager capacity schema omitted two states emitted for standard-UCI onboarding. The launcher now resolves `native/lichess_adapter_launcher.exe`; status requires live fresh evidence and exposes blockers; smoke delegates to the selected adapter; the closed schema includes every emitted capacity state. | Lichess commit `0f945b6`; contract/release checks, packaged standard-UCI smoke, stale-status tests, startup-path tests, and native launcher compile/probe evidence from an isolated removed build directory. | Source and published contracts are aligned for these seams. The native launcher test remains skipped in the clean source tree by design and must execute from the eventual immutable packaged artifact. |

## Audit progress

- [x] Freeze suite-level expected answers before implementation inspection.
- [x] Complete authoritative specification reading in all six repositories for the first broad pass.
- [ ] Generate and classify the complete tracked-file inventory.
- [ ] Review all first-party files line by line and populate the question ledger.
- [x] Run the first broad static boundary, schema, dependency, and ownership checks.
- [ ] Run focused unit, contract, substitution, and failure tests.
- [ ] Run packaged, installed, and end-to-end product acceptance.
- [ ] Complete the big-picture and technical assessments.
- [ ] Correct confirmed contradictions on the existing audit branches.
- [ ] Re-run affected validation and remove stale audit/build state.
- [ ] Commit, push, and open or update one review branch/PR per affected repository; do not merge to `main` until the whole audit and corrections are finished.
