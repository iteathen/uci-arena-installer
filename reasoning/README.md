# UCI Arena decision-reasoning system

Status: initial owner-directed reasoning architecture  
Suite owner: `uci-arena-installer`  
Behavior authority: existing accepted specifications/contracts remain authoritative in their owned domains.

## Purpose

UCI Arena uses an explicit reasoning graph so an AI agent cannot promote a local fix,
engineering convention, or convenient implementation into global architectural authority
without showing how it follows from the project's objectives and premises.

The graph is a directed acyclic reasoning structure, not a single-parent requirements tree.
A node may derive from several objectives, premises, assertions, or component exports.

The suite-level graph lives in `reasoning/suite_graph_v1.yaml`. Each product repository
owns a local `reasoning/component_graph_v1.yaml` projection. Local graphs import relevant
suite nodes and export component conclusions. Cross-component reasoning belongs in the
suite graph rather than being duplicated inside one component.

## Reasoning layers

Every material design decision should be traceable through:

1. **Objectives** — outcomes UCI Arena is actually trying to achieve.
2. **Premises** — currently accepted facts or working truths relevant to those objectives.
3. **Implicit assertions** — bounded causal/conditional consequences inferred when relevant
   premises are applied to objectives. They are not new world facts or hidden solutions.
4. **Implementation targets** — affirmative, solution-independent problems/conditions the
   resulting system should satisfy. A correctly formulated target is a successful reasoning
   result even when no implementation has yet been selected.
5. **Candidate solutions** — concrete mechanisms considered only after the upstream target is
   characterized. Existing code is one candidate/current implementation, not proof that its
   mechanism belongs in the target.
6. **Causal relations** — necessary-condition, enabling, feedback, or dependency relations
   among objectives/targets. Causal precedence is not automatically dominance.
7. **Dominance relations** — contextual Bayesian confidence that one objective/target should
   constrain or supersede another inside a genuine conflict scope.
8. **Importance relations** — contextual Bayesian confidence that one otherwise admissible
   objective/target deserves more optimization/attention in the stated context.

A candidate implementation is never justified merely because it fixes the immediate defect,
and an existing mechanism must never be back-projected into the upstream target definition.


## Coverage census before premise derivation

Before declaring premises complete, enumerate the broad concern areas that can materially
affect the current objectives. The suite census is `reasoning/concern_census_v1.yaml`.
Concern areas are a coverage map, not modules, repositories, or implementation boundaries.
Their purpose is to expose missing domains before a convenient architecture narrows attention.

A concern area belongs only when facts in that area could materially change attainment of
good chess playing, proving-ground validity, or usability. Do not add generic social,
economic, legal, or engineering topics without a concrete UCI Arena consequence.

## Premise strength and scope

In addition to Bayesian `confidence`, classify premises by the kind of proposition being
asserted:

- **structural** — a narrow logical, definitional, ownership, protocol, or physical/resource
  distinction;
- **empirical_regular** — a broad observed regularity whose frequency/magnitude may vary;
- **material_risk** — a credible possibility important enough to affect design even when it
  is not typical or inevitable.

The type is not a confidence score. A structural distinction can have uncertain application;
an empirical regularity can have high confidence but variable magnitude. Premise statements
must remain descriptive/epistemic and must not smuggle in a preferred algorithm, transport,
platform mechanism, module topology, or current implementation.

## Implicit-assertion discipline

Every implicit assertion records three distinct ideas:

- **derivation** — the premises and objectives that make the inference relevant;
- **inference class** — `structural`, `conditional`, or `risk_implication`;
- **boundary** — what the inference explicitly does *not* establish.

A boundary is mandatory reasoning, not prose decoration. It prevents a correct local
inference from becoming a wider unsupported authority. Assertions with no material target or
decision consequence are removed rather than preserved as general philosophy.

## Solution-independent target discipline

An implementation target states the problem to solve, not the current mechanism used to solve
it. Terms such as a particular transport, file format, operating-system integration,
programming language, process model, search backend, timing formula, or UI widget belong in a
target only when that mechanism is itself imposed by explicit higher authority.

For each target, ask: *Could two genuinely different architectures satisfy this target?* If
the answer is no only because the target embeds today's solution, rewrite it.

The reasoning stage succeeds when the problem is correctly characterized. Lack of a selected
solution, implementation, or proof of feasibility is not a defect in a well-founded target.

## Causal structure versus dominance

Do not infer dominance merely because A causally enables B. Record causal relations
separately. Dominance applies only after identifying a real decision scope in which two
objectives/targets cannot both be achieved as well as desired.

For a proposed conflict, record:

- the actual contested decision and conflict scope;
- the causal/necessary-condition paths;
- dominance direction;
- dominance strength: `strict_necessary_condition`, `strong_conditional`,
  `partial_defeasible`, or `unresolved_incomparable`;
- Bayesian confidence in that directional relation;
- evidence, counterexamples, and alternatives that might satisfy both sides.

If both objectives can be substantially satisfied by another design, there is no justified
sacrifice merely because the first candidate created a tradeoff. Dominance never licenses
gratuitous harm to the weaker side.

## Relevance boundary for implicit assertions

Do not attempt universal logical closure. Include an implicit assertion when its truth or
falsehood could materially change at least one of:

- candidate admissibility;
- a dominance relation;
- an importance relation;
- expected consequence to a current objective;
- required evidence;
- implementation selection.

Assertions outside that relevance closure are intentionally excluded.

## Confidence model

`confidence` is a value in [0,1] representing current Bayesian credence that the claim or
relation is correct **under its stated context**. It is not a utility score and, unless an
entry says otherwise, is not a calibrated frequentist probability.

Use provenance to distinguish explicit owner direction, accepted specification authority,
empirical evidence, strong derivation, and tentative inference. Do not manufacture decimal
precision from weak evidence.

Dominance is not absolute. A high-confidence dominance relation may be outweighed by a
sufficiently strong contrary case when no hard authority gate is violated. Importance is
likewise contextual and revisable.

`consequence` is kept separate from confidence. A lower-confidence catastrophic risk can
matter more than a high-confidence minor convenience gain.

Do not multiply confidence values as though graph edges were independent unless an accepted
analysis explicitly establishes that independence.

## Authority gates

Some rules are not Bayesian preferences. Explicit owner prohibitions, security/credential
boundaries, externally required protocol truth, and accepted hard constraints remain
authority gates until the authority that owns them changes them.

The reasoning graph operates inside that admissible envelope. It cannot use a confidence
score to waive a hard gate.

Existing accepted specifications/contracts own current behavior. When the reasoning graph
indicates that an accepted spec is no longer justified, the result is a **specification
review requirement**, not silent permission to ignore the spec.

## Decision procedure

For a material decision:

1. Identify the affected suite and component scopes.
2. Load the relevant suite nodes, local nodes, and interface edges.
3. State the candidate implementations.
4. Surface candidate-specific implicit assertions inside the relevance boundary.
5. Reject candidates that violate an authority gate.
6. Check that the relevant implementation targets are solution-independent; if a target
   merely restates today's mechanism, repair the target before comparing candidates.
7. Evaluate expected effects on the two root objectives and follow causal/enabling paths,
   including feedback and indirect effects.
8. Name any *actual* conflicts. Search for alternatives that satisfy both sides before
   invoking dominance.
9. For unavoidable material conflicts, apply contextual dominance with explicit scope,
   strength, confidence, evidence, and counterexamples.
10. Among remaining admissible candidates, apply contextual importance and consequence
    magnitude.
11. Prefer the candidate with the strongest traceable justification; do not use a generic
    engineering virtue or current implementation as an unstated objective.
12. Record new evidence, falsifiers, or changed confidence when the decision materially
    changes the reasoning graph.
13. If a local decision changes another component's assumptions or interface, update the
    suite graph before treating the local decision as complete.

## Self-reference limitation

This system improves reasoning but cannot make reasoning infallible. Agents must still reason
to identify objectives, premises, assertions, targets, and confidence updates. The structure
reduces risk by making those judgments explicit, reviewable, falsifiable, and invalidatable
rather than leaving them hidden in an agent's transient narrative.

Use independent passes when consequence warrants it: derive, challenge assumptions, search
for counterexamples, inspect cross-component effects, and reassess confidence.

## Canonical counterexample: move timing

The timing incident is retained as a reasoning-pattern example.

Evidence that one 500 ms block was useful and a future-capacity strategy was clock-safe
supported a bounded safety policy. It did **not** establish that one block was an optimal
general allocation, that additional blocks had no value, or that temporal value distribution
no longer mattered. Promoting that local safety result into general production timing
authority introduced unsupported implicit assertions and violated higher-level evidence-driven
timing targets.

The correct classification is: safe incumbent/fallback within its qualified scope, not
universal optimal timing authority.

## Maintenance

Reasoning nodes carry stable IDs. Change meaning by creating a successor node or explicitly
recording supersession; do not silently reuse an ID for a different claim.

Confidence updates should identify:

- old confidence;
- new confidence;
- new evidence or owner direction;
- affected downstream nodes;
- whether an accepted specification now requires review.

Historical failed assertions and rejected implementations remain useful evidence and should
not be erased merely because the current graph prefers another path.


## Premise taxonomy and completeness audit

Premises should describe accepted facts, epistemic limits, ownership facts, environmental
conditions, or user/context facts. A preferred implementation behavior is not a premise merely
because it is strongly desired; place normative design choices in implementation targets,
dominance relations, importance relations, or hard authority gates.

Use these premise classes when auditing coverage:

- **domain** — facts about chess, competition, clocks, engines, knowledge, or the proving
  environment;
- **epistemic** — what evidence can and cannot establish, including confounding, independence,
  statistical power, generalization, and overfitting;
- **composition/ownership** — which owner is authoritative for a semantic fact and how
  cross-component composition can change truth;
- **environment/resource** — hardware, concurrency, latency, resource contention, lifecycle,
  and other conditions that can materially change behavior;
- **operator/usability** — facts about user intent, configuration identity, discoverability,
  error interpretation, and consequential control.

A graph is premise-complete relative to the current objectives when:

1. every implementation target has a traceable derivation from at least one root objective
   plus the premises materially needed to justify that target;
2. every material cross-component interface has premises covering authority, identity/scope,
   lifecycle/change boundaries, and failure meaning where those can change a decision;
3. every comparative/qualification claim addresses material confounding, sampling/
   representativeness, independence, statistical power or inconclusive outcomes, and
   generalization scope;
4. every foreground-performance claim accounts for material runtime/resource/configuration
   conditions that can change the measured behavior;
5. every user-facing mutation that can alter experiment meaning accounts for operator intent
   and identity;
6. known counterexamples or historical failures that could change candidate selection are
   represented rather than erased;
7. no premise is merely a restatement of a chosen implementation.

Completeness is scoped, not universal. A newly introduced candidate can expose a new relevant
premise; that premise must be added before the candidate receives durable authority.

## Assertion relevance audit

Every implicit assertion must identify a material decision consequence. It belongs in the
active graph only when changing its truth could change one or more implementation targets,
candidate admissibility, evidence requirements, dominance/importance relations, or expected
objective outcome.

Graphs may record this with an `assertion_relevance` section mapping assertion IDs to the
targets or decisions they affect. An assertion with no such connection is out of scope and
should be removed rather than accumulated as general philosophy.

Two recurring epistemic rules are especially important for UCI Arena:

- failure to reject a difference is not evidence of equivalence unless an equivalence claim
  and its power/bounds were actually designed and qualified;
- the best candidate among tested candidates is evidence only about that tested candidate
  set and declared support, not proof of global optimality.
