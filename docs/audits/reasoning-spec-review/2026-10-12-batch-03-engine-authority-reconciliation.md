# Batch 03 — F01 legacy engine authority reconciliation

**Date:** 2026-10-12  
**Status:** Source and hierarchy evaluated; selected correction limited to accepted specification/architecture authority. Implemented code, release selections, historical receipts and component artifacts are not changed.  
**Predecessors:** [Batch 01 findings](2026-10-12-batch-01-suite-authority.md), [Batch 02 ranked candidates](2026-10-12-batch-02-candidate-decisions.md).  
**Repo baseline:** Installer `2af6be8157976fe800da223db88ab1bed45ea9af`. Read-only supporting GitHub source as of 2026-10-12; future users must recheck source HEADs before editing.

## Verified starting facts

1. [Product composition 0.3.63](../../../compositions/complete-arena-0.3.63.json) declares the `engine` role as `uci_arena.vector` from `iteathen/UCI-Arena-Vector`, and no `uci_arena.engine` source row. This is the latest inspected checked-in profile; it is **not** proof the corresponding release is signed or installed.
2. [Product composition validator](../../../tools/release/product-composition.mjs) admits exact `role/component_id/version/repository/required/default_enabled/requires` rows and requires declared dependency closure; `resolveComposition` keys by component ID, version **and repository**. No product identity is derived from a filename or obsolete static list.
3. [Release-lock v3 validator](../../../tools/release/release-lock.mjs) checks the exact component ID, source repository, version, ownership and dependencies against that selected composition. It fails on missing provider contracts, undeclared candidate repositories and incompatible rows.
4. [Specialized provenance contract](../../../specs/release_provenance_v1.yaml) expressly supersedes old GitHub-specific universal assumptions for new release work. Its v3 lock and trust-policy semantics are current; v1/v2 formats retain bounded compatibility meanings.
5. [Installation contract](../../../specs/installation_contract_v1.yaml) still has a static `component_candidate_repositories.uci_arena` entry and `signed_provenance.provider_candidates.trusted_providers.uci_arena` for `iteathen/UCI_Arena`; a historical `components.complete_arena` list contains `uci_arena.engine`; several release/validation examples still use native ONNX/CMake identity and `uci_arena_engine` variable names. GitHub resolves `iteathen/UCI_Arena` to the retained `iteathen/uci-arena-engine` repository, not to Vector.
6. [Canonical architecture](../../architecture/UCI_ARENA_COMPLETE_LEGO_ARCHITECTURE.md) already says Vector is the current chess/search product, explicitly cancels C++ engine deployment, and retains historical C++ illustrations. Its earlier example of a component manifest in §3 uses a historical `uci_arena.engine` executable without a nearby example-specific warning.
7. The retained native repository's active generic SDK/process/referee/timing contracts, older release provenance, historical receipts, and the `uci_arena_evidence.runtime` identity are **not** equivalent to present-day authority to select the cancelled native chess/search engine. Historical runtime input names and old model executables must not be rewritten casually.

## Reasoning hierarchy

**Objectives:** G-O1 (credible chess improvement/testing) and G-O2 (operable, reproducible deployment).  
**Premises and assertions:** G-P4 (correct parts can compose incorrectly), G-P5 (engine substitution), G-P6 (evidence identity/scope), G-P8 (local fix not global authority), G-P21 (behavior-altering configuration identity), G-P23 (one semantic owner) -> G-A3/G-A4/G-A5/G-A7/G-A9/G-A17. Installer I-P1/I-P2/I-P3/I-P6 reinforce explicit immutable provider and installed-state identity.  
**Targets:** G-T5 (neutral substitution), G-T6 (composition fidelity), G-T8 (recoverability), G-T12 (durable evolution), plus G-T4 (historical evidence truth) and Installer I-T1/I-T3/I-T5/I-T7.  
**Dominance:** G-D4 matters only if a local shortcut actually breaks composition truth; G-D7 matters only if convenience concretely hardcodes a single provider in a generic path. Neither is blanket permission to change legacy data. No unavoidable conflict requires sacrificing G-O1 or G-O2.  
**Hard gates:** Do not revive cancelled native chess/search, overwrite historical identities, bypass exact release selection/provenance, hardcode engine names into release tooling, or weaken validation or operator-selected composition.

## Compared solutions

| Candidate | G-T5/G-T6 fidelity | Legacy recovery/evidence | Change radius and risk | Disposition |
| --- | --- | --- | --- | --- |
| **A — Targeted current-source precedence and historical-scope annotations** | High: expresses existing exact composition/lock truth | High: old keys and data unchanged | Small, specification-only | **Selected** |
| **B — Split contract into current v3 spec plus separately indexed v1/v2/native annex** | High if migration is exhaustive | Potentially high; extra routers/consumers must migrate | Large, versioning/migration hazard | Retain if targeted annotations prove insufficient |
| **C — Replace every `uci_arena.engine` token and source with Vector** | Superficially simple but wrong for older receipts/ONNX-native checks | Low: destroys provenance and may falsify historical gates | Broad and unsafe | Rejected by historical truth gate |
| **D — Leave contract unchanged; trust code to ignore contradictions** | Current runtime may already be correct | Preserves history | Retains ambiguous agent authority | Rejected as incomplete review resolution |

**Why A wins:** The concrete release code already enforces source-of-truth by composition and lock. A makes the normative spec truthful without altering those tested mechanisms. B remains a genuine alternative if future audits uncover mutually incompatible required schemas. C fails history/recovery truth and D leaves the identified contradiction intact. This is a conditional ranking, not a fabricated numerical optimization result.

## Selected implementation boundaries

- Amend `specs/installation_contract_v1.yaml` with a **current selection precedence statement** that cites versioned Installer composition and exact compatible lock; identify current Vector product as an observed current choice, not an eternal hardcoded engine rule.
- Classify `iteathen/UCI_Arena`, static provider records, `components.complete_arena`, native ONNX/model release preflight and static executable-name examples as **historical/compatibility** within their actual specific scopes. Retain legacy keys, component IDs, release workflow identity, checks, historic receipts and model/runtime contract words unchanged.
- Clarify that the selected engine's default model is part of the selected exact component, not always `uci_arena.engine`. The selected current schema already has general release-lock composition rules; do not add a second selector.
- Add a contextual historical-example warning at the §3 native manifest illustration. No new Vector manifest is invented.
- Leave publication visibility and CI assumptions (F02) unchanged for the separate release-security audit. Do not sign, publish, run a release, change GitHub visibility, or alter runtime/Installer code.

## Verification and limitations

Diff review must establish that all source/component/provenance **literal identifiers used by compatibility readers** remain byte-identical and all changes are specification/document wording only. CI/schema checks are internal qualification; neither they nor this text establish native release or installation success. No user/operator credentials were read. No fresh deployment, Windows host qualification, or external reproduction was performed.

**Completion criterion:** One reviewed spec correction with no new source-selection mechanism, a successfully qualified PR, and a verified record in main. Any failure remains an explicit blocker rather than a claim of implementation success.

## Next bounded batch

**F02**: Review the transition from private repositories to public source/CI against actual producer release workflow protections, prerelease asset visibility, security/license obligations, `release_provenance_v1` trust-policy constraints, and retained historical draft identities. Re-rank the top F02 remedies only with newly verified gates. Keep release behavior unchanged unless a separate justified, scoped correction can be proven. Do not fold generic engine implementation research into that security decision.
