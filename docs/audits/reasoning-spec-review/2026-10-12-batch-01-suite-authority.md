# UCI Arena reasoning-hierarchy specification review — Batch 01

**Date:** 2026-10-12  
**Disposition:** Audit findings only; no accepted specification, runtime, release, or reasoning graph modified.  
**Scope:** Suite objectives and authority, canonical Installer-owned cross-component architecture, release/selection entry points, and one Book Forge-to-Vector evidence-authority edge. This is **not** an exhaustive review of all Installer specifications or any component's detailed implementation.  
**Evidence class:** Repository source and GitHub metadata reconciliation; no fresh native, deployment, security, or playing-strength qualification.

## Durable audit cursor

This is the first independently recorded bounded work packet in the post-reasoning-graph specification audit. On restart, read this file and the suite reasoning sources before proceeding; do not infer that unreviewed specs have been approved.

Baseline revisions actually inspected:

- Installer (suite owner): [c8a98c48](https://github.com/iteathen/uci-arena-installer/commit/c8a98c48bfa1dfedab29f256edbf162a3299f850).
- Book Forge: [319bbd8b](https://github.com/iteathen/uci-arena-book-forge/commit/319bbd8be3869aca982d726404046c2e91156b79).
- Vector composition target: [242c0e45](https://github.com/iteathen/UCI-Arena-Vector/commit/242c0e457cc5d6179b84f7436631ddce6fc98dcc); the component's detailed specifications were **not** audited here.
- Historical native repository identity: GitHub resolves iteathen/UCI_Arena to [iteathen/uci-arena-engine](https://github.com/iteathen/uci-arena-engine), not a nonexistent repository.

Sources examined: [suite reasoning methodology](../../../reasoning/README.md), [suite reasoning graph](../../../reasoning/suite_graph_v1.yaml), [concern census](../../../reasoning/concern_census_v1.yaml), [canonical suite architecture](../../architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md), [suite LEGO boundary](../../../specs/suite_lego_boundaries_v1.yaml), [Installer foundation](../../../specs/00_READ_FIRST.yaml), [installation contract](../../../specs/installation_contract_v1.yaml), [release provenance](../../../specs/release_provenance_v1.yaml), [current checked-in composition 0.3.63](../../../compositions/complete-arena-0.3.63.json), and [Book Forge accepted specification](https://github.com/iteathen/uci-arena-book-forge/blob/main/specs/opening_book_forge_v1.yaml). The latest checked-in composition is **not** asserted to be an installed, promoted, release-ready or running configuration.

### Method and authority

Both root objectives, **G-O1** (stronger chess play through trustworthy comparison) and **G-O2** (practical, understandable operation), have authority. Reasoning compares multiple candidate corrections inside existing hard security, credential, provenance, and accepted behavioral constraints. The graph does not retroactively overrule an accepted spec.

The most relevant suite derivation paths are G-P4 / G-P5 / G-P21 / G-P23 -> G-T5 / G-T6 / G-T8 / G-T12 (correct replaceable composition); G-P6 / G-P18 / G-P28 -> G-T4 / G-T13 (truthful evidence and qualification); and G-P21 / G-P23 -> G-T14 (operator/security authority). Contextual relations G-D2, G-D3, G-D4, G-D7, and G-D8 apply **only in their named conflict scopes**. Do not treat causal dependencies as blanket dominance.

## Findings and competing corrections

### B01-F01 — HIGH specification-authority drift: native source versus current Vector composition

**Evidence:** The active [installation contract lines 7–35](../../../specs/installation_contract_v1.yaml#L7-L35) lists the redirected legacy repository iteathen/UCI_Arena as a component candidate source, including uci_arena.engine, while also listing Vector. [Its component selection list and rules](../../../specs/installation_contract_v1.yaml#L1189-L1241) still name uci_arena.engine among Complete Arena components. By contrast, [current checked-in composition 0.3.63](../../../compositions/complete-arena-0.3.63.json) selects uci_arena.vector from iteathen/UCI-Arena-Vector; the [canonical architecture lines 7–13 and 49–58](../../architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md#L7-L13) explicitly cancel C++ engine deployment and recognize Vector as the current product. The legacy name **resolves to the historical native repo**; it is not proof of a missing remote. The current composition is evidence of selection intent, not installed runtime qualification.

**Hierarchy consequence:** A future agent could promote native historical release assumptions into a current Installer requirement or select competing engine artifacts, degrading G-T5 and G-T6. This is a specification/navigation conflict, **not** evidence that the current installer actually selected the wrong executable.

**Candidate approaches:**
- A — Mark native component and repository entries as historical/compatibility-only and state unequivocally that current suite composition supplies deployment identity. Preserve legacy wire identities and release history. **Leading candidate** for bounded reconciliation.
- B — Version/split the installation contract into a current Vector/release authority and a precisely scoped legacy compatibility annex. Better durable source-of-truth separation, but higher migration/review surface.
- C — Leave it untouched because checked-in compositions are already correct. Lowest immediate effort but retains contradictory future agent instructions; not preferred.

**Review disposition:** Require precise precedence and compatibility mapping before editing; avoid mechanically deleting old component names that may still describe historical receipts or retained compatibility artifacts. Verify actual current release tooling against composition before authorizing behavior edits.

### B01-F02 — HIGH publication and release-security authority drift after public conversion

**Evidence:** [Installation contract lines 39–171](../../../specs/installation_contract_v1.yaml#L39-L171) declares private repository visibility and immutable **private** candidate releases/mirrors; [lines 752–879](../../../specs/installation_contract_v1.yaml#L752-L879) describe CI disabled for private repositories while also documenting pull-request CI. GitHub repository metadata now identifies Installer, Vector, Manager, Lichess Bot, Book Forge, Evidence Service, and Syzygy root-provider as public. GitHub Actions CI is enabled on the newly public repositories and protected required checks exist (previously independently checked). The [latest Manager published prerelease](https://github.com/iteathen/uci-arena-manager/releases/tag/component/manager-core/v0.3.29/windows-x86_64) and [Book Forge published prerelease](https://github.com/iteathen/uci-arena-book-forge/releases/tag/component/book-forge/v0.4.10/windows-x86_64) are **non-draft**, with assets; publishing a prerelease in a public repository is not a private distribution channel. Syzygy's observed intake releases remain **draft**, which must not be conflated with published prereleases. [Specialized release provenance](../../../specs/release_provenance_v1.yaml#L7-L28) explicitly supersedes GitHub-specific universal assumptions where applicable.

**Hierarchy consequence:** G-T6 / G-T8 / G-T14 require trustworthy, nonmisleading distribution and provenance across lifecycle changes; an owner-authorized move to public source does **not** automatically approve unrestricted candidate-asset contents or change the meaning of signed claims. No credential or asset-content disclosure is claimed here: a full sensitive-content and release-asset review was **not performed in this batch**.

**Candidate approaches:**
- A — Reconcile accepted visibility/CI language and release publication gates to present public source plus exact immutable/provenance-verified release candidates; perform explicit asset/security review and keep manual signing/promotion authority. **Leading candidate subject to security gate.**
- B — Keep public source/CI but route embargoed candidates through a separately authorized private transport/release owner. Stronger isolation where required, at added authorization, configuration, and trust-chain complexity.
- C — Reprivatize source solely to restore old release assumptions. Contradicts the owner's public-CI decision and sacrifices usability; **reject absent a separate material security finding**.

**Review disposition:** Security/release-authority review required before deciding whether existing publication channels are appropriate. Do not run signing, publication, release mutation, visibility changes, or secret scans that expose data. Distinguish repo visibility, draft visibility, published prerelease visibility, CI, and official stable promotion.

### B01-F03 — MEDIUM suite reasoning edge overstates Book Forge qualification scope

**Evidence:** [G-X2 in suite graph](../../../reasoning/suite_graph_v1.yaml#L766-L772) states that Book Forge produces “qualified immutable opening knowledge.” But [Book Forge publication contract](https://github.com/iteathen/uci-arena-book-forge/blob/main/specs/opening_book_forge_v1.yaml#L188-L234) distinguishes (1) an automatically produced, fixture-permitted, runtime-consumable **integration** channel explicitly labeled **not strength qualified**, from (2) a separately gated **qualified** channel. The integration channel is intentionally useful without a playing-strength claim.

**Hierarchy consequence:** G-P6 / G-P28 -> G-T4 and contextual G-D2 / G-D8 prohibit importing unqualified evidence into a broader strength/optimization assertion. This is an **overbroad graph statement**, not a finding that current Book Forge falsely promotes integration output.

**Candidate approaches:**
- A — Make G-X2 channel-conditional: Book Forge supplies immutable knowledge with its declared integration/qualified authority; Vector verifies and consumes each channel only within its stated scope. **Preferred**: keeps usability and truthful evidence together.
- B — Require strength qualification before any operational book consumption. Stronger default gate but removes intended integration usefulness, without evidence that this is necessary for ordinary experimental operation.
- C — Retain G-X2 and depend on every consumer to reinterpret “qualified.” Leaves suite-level reasoning misleading.

**Review disposition:** Recommend a narrowly worded graph-edge review. The existing two-channel contract retains behavioral authority until separately changed. No evidence claim is upgraded by this audit.

### B01-F04 — LOW/MEDIUM topology vocabulary gap: six product repositories versus independent Syzygy owner

**Evidence:** [Architecture lines 31–45](../../architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md#L31-L45) enumerate six product repositories, while [suite graph G-X8 and component_graphs](../../../reasoning/suite_graph_v1.yaml#L816-L848) include independent iteathen/uci-arena-syzygy-root-provider. [Current composition 0.3.63](../../../compositions/complete-arena-0.3.63.json) selects syzygy.root-provider as a separate optional component.

**Hierarchy consequence:** G-P4 / G-T6 / G-T12 favor explicit component versus product and lifecycle classification; this **may be a naming/coverage difference rather than a contradiction**, since a separately owned provider need not be counted among the six core products.

**Candidate approaches:** A — clarify “six core products plus separately owned optional Syzygy provider” in suite topology (**provisional preference**); B — explicitly designate Syzygy a seventh product if its ownership/lifecycle warrants that classification; C — continue leaving its relationship implicit. No repo split or merger is justified on this evidence.

## Checked and not promoted to findings

- The [canonical suite architecture's opening note](../../architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md#L7-L13) already labels historical C++ sections 4–13 as illustrations and disallows using them to specify Vector or revive a native engine fallback. These sections are **not automatically live defects**.
- [Release catalog lock](../../../release/catalog.lock.json) is a historic 0.3.9 document with release_ready=false. Its age is **not** proof of a currently deployable wrong release lock.
- No reviewed evidence establishes current suite release qualification, native runtime behavior, secure status of all existing release assets, or chess-strength improvement.
- This pass did not change accepted specifications or assess all contract schemas, code implementations, and per-component qualification evidence.

## Batch completion and handoff

**Completed scope:** Suite reasoning authority, Installer top-level architectural and release-source assumptions, current checked-in composition, and Book Forge G-X2 qualification boundary. **Four findings:** two high-priority authority-review requirements, one evidence-scope correction, one terminology/classification question.

**Next bounded batch:** Installer active specialized specifications: compare installation_contract_v1 against release_provenance_v1, install_transaction_v2 and effect companions, layout contracts, artifact acquisition, provider lifecycle, install preservation/rollback, and agent/spec registry routing. Preserve F01 and F02 without re-litigating this packet; promote only new source-grounded findings. Later batches must separately review Vector; Manager; Lichess Bot; Book Forge; Evidence Service; Syzygy; surviving native generic contracts; then cross-component edge reconciliation. Do not label the entire specification inventory reviewed until all owners and relevant cross-edges are checked.

**Writing policy:** Subsequent batches create separately reviewable dated files in docs/audits/reasoning-spec-review/, each recording exact repos/revisions, files actually read, hierarchy trace, competing strategies, evidence limits, and next cursor. Do not modify this completed packet's factual historical observations when later revisions supersede them; issue an addendum/correction instead.
