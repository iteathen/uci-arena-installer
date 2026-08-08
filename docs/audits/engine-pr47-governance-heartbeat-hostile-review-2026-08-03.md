# Engine PR #47 hostile review — governance and lease heartbeat

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/UCI_Arena`  
PR: #47 `agent/integrate-engine-audit-stacks`  
Reviewed head: `60f7bc9fe64dd1c405bcc6009232ecb875b760ef`  
Base: Engine PR #44 / `agent/model-adapter-contract` at `d37ab900486cad25b5dff5cd6fa11724c8fc3929`  
Formal review: `4850880052`  
Disposition: **changes required; blocked parent; governance reconstruction**

## Confirmed governance blockers

1. `specs/index.yaml` declares every unregistered document non-authoritative while `tools/spec_preflight_check.py` still mandates many omitted contracts, including native, model, deployment, canonical-state, evaluation, dependency, and improvement-plan specifications.
2. AGENTS and `00_AI_READ_FIRST` require every preflight result to be followed, making compliance undecidable when preflight-returned documents are simultaneously deauthorized.
3. Roughly 1,500 registry lines were removed without a complete migration register proving each contract active, replaced, moved, historical, or deletable.

## Confirmed heartbeat blockers

1. Lease loss is recorded but protected work is not cancelled; it can continue arbitrary side effects after authority is lost and only fails when returning.
2. The alleged typed failure is an unversioned free-form string/generic Error; diagnostics are truncated but not sanitized.
3. `startResourceLeaseHeartbeat` and changed foreground semantics were added while the package remained version 1.0.2, so materially different APIs share one version identity. The PR body incorrectly says 1.0.1.
4. Failed monitors retain a scheduled interval until callers explicitly stop them; stopped/failed/assertCurrent semantics conflict.
5. Interval values are insufficiently bounded against Node timer range and lease stale policy, with no immediate authority check or deterministic timer/clock seam.
6. Callback, work, heartbeat, and release failures do not preserve a typed primary-failure/cleanup-causality record.
7. Sleep-based tests do not prove cancellation, timer cleanup, stop/failure races, interval bounds, failure causality, or no side effects after lease loss.

## Required correction

- reconcile registry and preflight through an explicit migration register and automated authority-graph validation;
- preserve the concise Engine-only governance identity but restore every current Engine contract to one named authority;
- version heartbeat authority as a typed cancellation/lifecycle contract;
- stop/cancel protected work promptly on lease loss;
- self-clean terminal timers and preserve primary failure plus bounded cleanup diagnostics;
- bump package identity and rebind consumers/vendor locks;
- reconstruct only after corrected PR #44 model-adapter ancestry.

## Audit consequence

PR #47 is not a valid authoritative Engine base. All descendants—PR #52 and active Jules PR #63 included—are parent-blocked even if their local slices become correct. Correct child work should be preserved as replayable patches/evidence, not merged on current ancestry.