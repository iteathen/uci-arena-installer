# Reasoning-hierarchy candidate comparison — Batch 02

**Status:** Source-backed candidate comparison and bounded implementation record. **Date:** 2026-10-12.
**Predecessor:** [Batch 01 findings](2026-10-12-batch-01-suite-authority.md).
**Baseline:** Installer main `0ff9cb7395841a5647cb2fa9373525b1be8bd8ac`; Book Forge `319bbd8be3869aca982d726404046c2e91156b79`; Vector `242c0e457cc5d6179b84f7436631ddce6fc98dcc`. Recheck source revisions before subsequent changes.

## Method

Root objectives: **G-O1**, truthful progress toward stronger chess through a trustworthy proving ground; **G-O2**, operator-usable and efficient proving ground. Premise/assertion and target derivation follows `reasoning/suite_graph_v1.yaml`, `reasoning/README.md`, local component graphs, and accepted owner specifications. Hard gates (security, ownership, provenance, user intent, and accepted contract meaning) eliminate inadmissible variants before ranking. No candidate is a mandate merely because it has fewer edits. Causal dependencies are not global dominance.

Ranking dimensions: **objective fidelity**, **truthfulness and evidence authority**, **cross-component ownership**, **reversibility/compatibility**, **change radius and operational cost**. There is no manufactured utility score or multiplication of subjective confidence values. Dominance G-D2, G-D4, G-D8 applies only where its named conflict is real. If an alternative satisfies competing goals, avoid invoking dominance to justify an unnecessary sacrifice.

## B01-F01 — Historical native engine assumptions in current Installer authority

**Decision target:** Give agents one current deployment selection and preserve historical compatibility without allowing obsolete C++ native engine artifacts to become a substitute for Vector. G-P4, G-P5, G-P6, G-P21, G-P23 -> G-T5, G-T6, G-T8, G-T12; Installer I-P1, I-P6 -> I-T1, I-T5. **Actual contested scope:** Avoiding parallel/ambiguous deployment authority, not choosing one engine algorithm. Explicit current Vector ownership in `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md` is a binding product decision.

1. **F01-A — Targeted current-vs-history annotation and composition-driven selection (rank 1; selected pending packet).** Explicitly classify `iteathen/UCI_Arena` (redirecting to `uci-arena-engine`) and `uci_arena.engine` as historical/compatibility where used by prior locks/receipts; current deployments read the selected versioned `compositions/complete-arena-*.json`. Retain legacy schema fields and provenance identities. Best preserves G-T5, G-T6, G-T8, G-T12 with small radius; requires a literal-reference audit before edits.
2. **F01-B — Versioned active-spec split with legacy annex (rank 2).** Cleaner ongoing separation but large cross-document and validator migration footprint; appropriate if incompatible schemas cannot coexist safely.
3. **F01-C — Rely on composition and leave the active spec unchanged (not selected).** Low immediate cost but fails to remove contradictory agent-facing current authority.

**Falsifier/gate:** If current release tooling actually requires the old record's fields as live constraints, annotation alone may not suffice. No release-selection code, historic receipt, source identity, or component identifier may be changed based only on this audit.

## B01-F02 — Repo visibility/CI and release channel authority drift

**Decision target:** State the truth about public source and CI while retaining exact signing, provenance, trust, release approvals and any genuinely private transport boundaries. G-P6, G-P21, G-P23, G-P28 -> G-T4, G-T6, G-T8, G-T14; contextual G-D3 (credential authority), G-D4 (composition truth). The user approved public repositories; **that was not a waiver of secret isolation or release authorization**.

1. **F02-A — Explicitly separate visibility, automation, candidate publication, and promotion (rank 1; selected subject to security gate).** Update stale repo-private/CI-disabled assertions as current public-CI facts. Published prerelease assets in a public repository are public; GitHub drafts are not automatically published. Continue digest/attestation/issuer/source/branch/trigger/approval checks and review asset exposure. Make production trust independent of a repo being private unless a separately stated embargo requires privacy. Preferred for G-O1/G-O2 and G-T6/G-T14 without diluting actual security.
2. **F02-B — Public source and CI with a separately authorized private candidate-asset owner (rank 2).** Stronger embargo if proprietary artifacts/keys/unsafe assets warrant it, but new trust and operational edges; cannot assume such necessity from old prose alone.
3. **F02-C — Make source private and disable CI to restore historical wording (not selected).** Conflicts with the owner's current public-repo direction, loses CI benefits, and does not solve release truth.

**Evidence and limitation:** GitHub REST identifies public repositories and non-draft published component prereleases; GitHub documents that published releases are viewable by users with repository read access. This is not a secret/content scan. Specialized `specs/release_provenance_v1.yaml` supersedes older GitHub-specific assumptions for new signing and lock work. **Implementation held to its own bounded packet** pending exact mapping of legacy references, current workflows, asset authority and compatibility. Do not loosen proof gates or convert draft Syzygy releases to published.

## B01-F03 — Book Forge integration is not automatically playing-strength-qualified

**Decision target:** Model the precise owner-to-consumer knowledge authority: installation/integration usefulness and stronger playing-strength qualification are distinct. G-P6, G-P8, G-P18, G-P28 -> G-T4, G-T5, G-T6, G-T11, G-T13; Book Forge B-P2 / B-A10 -> B-T4 / B-T6. G-D2 and G-D8 prevent promoting weaker evidence to general qualification, **not** using explicitly labeled integration artifacts.

1. **F03-A — Channel-conditional G-X2 interface statement (rank 1; selected for this packet).** Book Forge publishes immutable, explicit integration or strength-qualified snapshots; integration may be used within its admitted scope without a strength claim; Vector verifies/consumes declared identity and authority without private producer logic. Meets both objectives without a forced tradeoff.
2. **F03-B — Require strength qualification before *any* operational opening-book use (rank 2, not selected).** Makes claims conservative but unnecessarily removes the accepted and intentionally useful integration channel; no demonstrated hard gate forbids integration use.
3. **F03-C — Keep “qualified immutable opening knowledge” and rely on implicit consumer reinterpretation (rank 3, not selected).** Leaves graph-level claim overbroad; contrary to G-T4 evidence truth.

**Proof sources:** `reasoning/suite_graph_v1.yaml` G-X2; [Book Forge publication channels](https://github.com/iteathen/uci-arena-book-forge/blob/main/specs/opening_book_forge_v1.yaml); [Vector Book binding/admission](https://github.com/iteathen/UCI-Arena-Vector/blob/main/docs/development/2026-10-09-installed-book-binding.md). **Change boundary:** G-X2 wording only; preserve G-X2 ID, graph semantics, links, supported targets, Book Forge publication gates and consumer behavior.

## B01-F04 — Six core products and independent optional Syzygy provider

**Decision target:** Make component topology exhaustive for cross-owner reasoning without redesigning the repository or conflating current install selection with qualification. G-P4/G-P5/G-P23 -> G-T5/G-T6/G-T12; G-X8 and Syzygy S-P2/S-P5 -> S-T6/S-T8. No demonstrated unavoidable objective conflict.

1. **F04-A — Six core products plus explicitly separate optional root-provider owner (rank 1; selected for this packet).** Amend only the architecture's topology description to include the independent Syzygy repo as an optional edge and clarify that product and repository counts differ. No lifecycle/installation/qualification claim added.
2. **F04-B — Promote Syzygy to a seventh core product (rank 2, conditional).** Could be correct after an explicit owner classification and lifecycle decision, but overstates current core status without such evidence.
3. **F04-C — Leave the Syzygy reference only in distant graph/component lists (rank 3).** No runtime defect demonstrated, but obscures the cross-owner topology.

**Proof sources:** `docs/architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md` topology; `reasoning/suite_graph_v1.yaml` G-X8; `compositions/complete-arena-0.3.63.json`; [Syzygy provider owner](https://github.com/iteathen/uci-arena-syzygy-root-provider/blob/main/AGENT_LOCAL.md). The composition is current **planning metadata**, not proof Syzygy signed, installed, operationally qualified or promoted. **Change boundary:** architecture topology only; retain six core-product classification and historical engine references.

## Implementation order, verification and stop point

**This packet implements:** F03-A and F04-A (small read-only semantic corrections in suite-level owner files). They share a single suite reasoning/architecture owner and no downstream production behavior. The top-ranked options for F01 and F02 remain recorded for the next packets because current release/provenance/lifecycle gates need a dedicated review.

**Acceptance:** Graph remains a directed acyclic reasoning dependency graph with unchanged IDs and edges; actual accepted Book Forge channel contract preserved; topology reflects Syzygy without claiming qualification; diff contains only this note, one G-X2 statement and the suite architecture topology prose/diagram; required CI and post-merge checks pass. No tests from this packet qualify engine strength, signing, public asset safety, or Installer behavior.

**Next packet:** F01-A source-provenance and component-selection reconciliation; then F02-A visibility/CI and candidate asset trust reconciliation under a separate security/compatibility scope. Restart from this file and Batch 01; never treat a selected but not implemented candidate as completed.
