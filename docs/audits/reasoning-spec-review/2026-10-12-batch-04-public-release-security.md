# Batch 04 — F02 public repositories, CI, and release-trust authority

**Date:** 2026-10-12  
**Disposition:** Completed bounded analysis and selected **specification posture reconciliation only**. Source/release security review, historical drafts and publication authorization remain independent gates; this note does not qualify released assets as safe.  
**Predecessors:** [Batch 01](2026-10-12-batch-01-suite-authority.md), [Batch 02 candidate comparison](2026-10-12-batch-02-candidate-decisions.md), [Batch 03](2026-10-12-batch-03-engine-authority-reconciliation.md).  
**Installer baseline:** `1857d5150563c23e905452e91080d388a4f93826`. Public repository and release metadata were checked via live GitHub on 2026-10-12.

## Evidence and separation of authorities

- **Source visibility:** Installer, Manager, Lichess Bot, Book Forge, Evidence Service, Vector and Syzygy root-provider repositories are currently GitHub **public**. Their older private-only documentation is stale as a present-tense source-visibility claim.
- **CI:** Installer, Manager, Lichess Bot, Book Forge, and Evidence Service currently have `.github/workflows/ci.yml` with `pull_request` and `push` triggers, `contents: read`, and no direct release job. Syzygy's checked-in workflow is a manually dispatched component release plus non-release verification; Vector has its own PR/push quality and explicit manual signing paths. CI completion is **internal qualification** only; it does not itself sign, attest, promote or validate public asset contents.
- **Producer release:** Manager, Lichess Bot, Book Forge, Evidence Service and Installer-managed Node/Stockfish component candidates have existing GitHub releases with `draft=false, prerelease=true` and downloadable named assets. Since these repositories are public, **the published prerelease assets are public**, notwithstanding historical wording that calls them private. This does not mean they are stable/suite-approved. Candidate release workflows are triggered by `workflow_dispatch`; signing steps and write/id-token grants are distinct from the ordinary read-only CI jobs.
- **Syzygy:** Its 2.1.0/2.1.1 intake and component candidate releases are currently **draft** (not publicly published releases). Its [source intake validator](https://github.com/iteathen/uci-arena-syzygy-root-provider/blob/main/tools/release.mjs) explicitly requires `metadata.repository.private === true` and visibility `private`; the repo is now public. Thus the old private-only signed-candidate path is **incompatible with the present repository visibility** and currently fails closed. The root [owner restrictions](https://github.com/iteathen/uci-arena-syzygy-root-provider/blob/main/AGENT_LOCAL.md) prohibit unsanctioned public visibility/signing/release changes. Do **not** remove its private gate, publish drafts or fake successful release readiness in this packet.
- **Current release authority:** [Installer release provenance v1](../../../specs/release_provenance_v1.yaml) expressly supersedes GitHub-specific universal assumptions in [installation_contract_v1](../../../specs/installation_contract_v1.yaml) for new release work. Its `arena_suite_release_lock_v3` binds exact producer trust policy, scope, source, artifact, evidence and signature, independent from release-byte transport. It does **not** grant release authorization simply because files are public.
- **Old release transport:** The installation contract still contains a keyless/private GitHub release/mirror/v1/v2 lock route, a `repository_visibility: private` field, and claims that hosted PR CI is disabled. Those must be classified as **historical GitHub/private-workflow semantics** rather than asserted as live universal policy. The Installer current tree has a `managed-component-release.yml` manual candidate workflow and `ci.yml`; it does not contain the legacy referenced `.github/workflows/release.yml`, so this review asserts no completed official suite publishing workflow.
- **Security limits:** No full Git-history secret scan, source licensing audit, historical release-binary content audit, deployment, signing, or independent hostile security review occurred here. Public source and release metadata alone do **not** establish clean assets or safe exposure. Incident-class findings, if discovered later, belong on an approved private channel rather than a public PR.

## Decision against the reasoning hierarchy

**Objectives:** G-O1 trustworthy competitive evidence depends on exact reproducible deployed identities; G-O2 includes reliable, comprehensible publication/updates.  
**Premises:** G-P4, G-P6, G-P8, G-P20, G-P21, G-P23, G-P28 and Installer I-P1/I-P6/I-P7. Reusable factual distinctions: visibility is not signing provenance; CI is not a signing oracle; a published prerelease is not a confidential channel; a draft is not stable release authority.  
**Assertions:** G-A3/G-A5/G-A7/G-A17/G-A26; Installer I-A6/I-A7.  
**Targets:** G-T4 (correct evidence claim), G-T6 (cross-component/trust compatibility), G-T8 (release/recovery integrity), G-T12 (change radius), G-T14 (secrets and operator authorization); Installer I-T1/I-T3/I-T5/I-T9.  
**Conditional dominance:** G-D3's user/credential authority over convenience is a hard boundary only when convenience would actually weaken it; G-D4 when local shortcuts break composed truth; G-D8 if fast publication requires unqualified claims. No blanket ban on public source or safe published candidates follows from these relations.  
**Hard gates:** Do not leak credentials; equate CI success with attestation; weaken source/manifest/lock/signer scopes, workflow identity, main-only signing constraints, immutable artifacts or release approval; publish Syzygy drafts; or change repository visibility without separate owner authority.

### Candidate comparison

| Candidate | G-O1 evidence/provenance | G-O2 operational truth | Hard-gate / change risk | Disposition |
| --- | --- | --- | --- | --- |
| **A — Explicit separation of current public source+CI, published candidate visibility, and cryptographic promotion; preserve legacy private-contract scope** | Preserves exact trust and identifies what public bytes can and cannot prove | Removes misleading current descriptions; no runtime migration | Low for narrowly scoped authority/doc updates; asset and Syzygy security gates remain | **Selected for this packet** |
| **B — Public source/CI with separate approved private candidate-asset transport** | Can provide embargo while preserving signing | Adds transport/operator complexity, future dependence | New private ownership, credentials and policy; justified only by specific embargo/security finding | Conditional reserve |
| **C — Reprivatize source and disable CI** | No assurance of asset-content safety, and already-public history cannot be unexposed | Contradicts explicit owner direction and loses public CI | Requires new owner authorization; not a remedy to current trust semantics | Rejected |
| **D — Make all releases immediately public and remove old private/draft guards (including Syzygy)** | Can collapse evidence/approval and confidentiality scopes | Superficially simple but creates unauthorized publication | **Fails** Syzygy hard authority, signing/asset-review boundaries | Rejected |

**Why A:** The actual read-only CI and signed release roles already have separable permissions. A truthful owner-spec reconciliation can remove contradictions without adding a second trust or release system, changing historical evidence, or weakening critical boundaries. B is retained when material asset/private-transport evidence justifies it. C and D fail real owner/security gates.

## Implementation selection and checkpoints

**Implemented in this packet:** Installer-owned active `specs/installation_contract_v1.yaml` source/CI/release scope statements, with specialized `specs/release_provenance_v1.yaml` as the authority for current trust and v3. Do not change its existing historical v1/v2 field names, matching old release identity, release-tool source code, workflows or verified candidate bytes.

**Unresolved — explicit follow-up:** 
1. Book Forge and Evidence Service have `specs/repository_boundary_v1.yaml` declarations `visibility: private` and `automatic_ci: disabled` that contradict live metadata; update each under its owner's separately reviewed packet. Any other provider's release docs must be similarly classified by actual authority and history, not globally replaced.
2. Syzygy independent owner/security review must decide how current public source can retain draft intake confidentiality and authorized signing. Do not weaken `tools/release.mjs` private gate or release drafts based on this batch.
3. Independently review exposed **published** candidate-asset inventories, packaging obligations, and relevant histories for sensitive material; absence of findings cannot be claimed until reviewed. Keep sensitive incident evidence private.
4. Verify live release governance and optional suite publication path before asserting official Installer stable release readiness. The missing historical `release.yml` cannot be inferred from legacy specification.

**Validation:** PR diff must change only documented Installer release/CI posture and this decision note; retain actual cryptographic release hard gates, identity rules, historical records and owner authority. Existing required CI/CodeQL qualify source/spec integrity only. This is not security approval for previously published artifacts.

**Next bounded batch:** Owner-scoped stale Book Forge and Evidence Service release/CI documentation reconciliation; separately queue Syzygy release transition for owner/security review. Do not silently resolve the restricted Syzygy gate on a general documentation PR.
