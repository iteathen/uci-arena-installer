# Installer Delegation Contract

Status: active execution policy for the Installer hardening program while lower-capability implementation agents are used.  
Applies to: the architecture and issue set described by `INSTALLER_HARDENING_ARCHITECTURE.md`.

## 1. Principle

Delegation happens **after architecture**, never instead of architecture.

The organizing Sol-class model owns LEGO and SOLID. A delegated Spark-class model may apply CUPID and KISS only inside a boundary whose LEGO ownership and SOLID design have already been fixed.

## 2. Non-negotiable design hierarchy

The design doctrines are not peers and are not freely traded against one another. They are a strict nesting hierarchy:

```text
LEGO
  owns system/product bricks, ownership, lifecycle, replacement, and failure boundaries
  |
  `-- SOLID
        owns responsibilities, public interfaces, substitution, and dependency direction
        inside the selected LEGO brick
        |
        `-- CUPID
              owns implementation qualities inside the fixed SOLID design
              |
              `-- KISS
                    selects the simplest complete implementation among options
                    that already satisfy LEGO, SOLID, and CUPID
```

Equivalently:

```text
KISS is always inside CUPID.
CUPID is always inside SOLID.
SOLID is always inside LEGO.
```

This ordering is authoritative:

1. First reject any option that violates LEGO ownership, lifecycle, replacement, or failure boundaries.
2. From the remaining options, reject any option that violates the fixed SOLID responsibility, interface, substitution, segregation, or dependency design.
3. From the remaining options, reject any option that violates CUPID implementation quality.
4. Only then use KISS to choose the least complex complete implementation.

A lower layer can never justify violating a higher layer:

- “simpler” code is invalid if it is not CUPID;
- a locally attractive CUPID design is invalid if it changes the fixed SOLID contract;
- a SOLID-looking abstraction is invalid if it crosses or duplicates a LEGO owner;
- no implementation convenience may create a second truth owner, lifecycle owner, registry, transaction engine, path authority, release authority, or provider-specific branch in Installer.

When a lower-level implementation cannot satisfy a higher-level boundary, the result is `blocked_architecture`. The delegated agent must not reinterpret, weaken, or route around the higher-level design.

## 3. Responsibility flow

```text
Sol-class organizing model
  -> LEGO ownership/component boundaries
  -> SOLID responsibility/interface/dependency design
  -> schemas/contracts/invariants
  -> issue decomposition and dependency order
  -> exact implementation slice and tests
                     |
                     v
Spark-class implementation model
  -> CUPID implementation quality inside the fixed SOLID design
  -> KISS selection inside CUPID-compliant alternatives
  -> idiomatic local code
  -> focused tests/evidence for the assigned slice
                     |
                     v
Sol-class organizing model
  -> review in LEGO -> SOLID -> CUPID -> KISS order
  -> architectural reassessment
  -> integration decision
  -> parent-issue status
  -> next implementation slice / next_step
```

## 4. Sol-owned decisions

The organizing model decides before any delegated implementation begins:

- what the meaningful LEGO bricks are;
- which repository/component owns each responsibility and lifecycle;
- replacement and failure boundaries between bricks;
- whether an existing owner must be reused rather than creating another system;
- Single Responsibility boundaries at component/module level;
- Open/Closed extension points that are actually required by known variation;
- Liskov-substitutable public behavior and failure semantics;
- Interface Segregation: the smallest complete role contracts exposed at boundaries;
- Dependency Inversion: which capabilities are ports and which concrete systems are adapters;
- public/project-owned types, schemas, IDs, persistence meaning, and compatibility rules;
- dependency direction and allowed cross-brick calls;
- transaction/lifecycle state machines and points of no return;
- security, provenance, destructive-operation, preservation, and recovery invariants;
- issue relationships, prerequisites, exclusions, and completion criteria;
- which exact files/interfaces an implementation slice may touch;
- the focused falsifiers/tests that prove the slice;
- whether a result is integrated, revised, rejected, or followed by another slice;
- `next_step`.

These decisions are not delegated merely because the delegated model sees a locally attractive refactor.

## 5. Spark-owned decisions

Inside the already-fixed LEGO brick and SOLID interface, Spark may decide local implementation details using CUPID and then KISS.

CUPID is applied as follows:

- **Composable:** private pieces compose through the fixed interfaces without introducing a competing public extension point or owner.
- **Unix:** each private helper does one bounded job, data flow is explicit, and outputs are usable by the next fixed step.
- **Predictable:** behavior, ordering, errors, cleanup, and resource bounds are unsurprising and testable.
- **Idiomatic:** implementation follows the language, standard library, and repository conventions unless the fixed contract requires otherwise.
- **Domain-based:** names and private structures reflect the already-defined Installer domain rather than framework or provider internals.

KISS is applied only after those CUPID conditions are met. Spark may:

- choose straightforward private helper structure;
- use idiomatic language/library constructs;
- reduce incidental complexity and duplication within the assigned owner;
- keep control flow comprehensible and predictable;
- keep private names, functions, and data structures easy to use correctly;
- keep error handling explicit and unsurprising;
- implement tests and fixtures required by the assigned contract;
- choose the smallest private abstraction that completely satisfies the fixed behavior.

KISS does not mean fewer invariants, weaker validation, hidden failure, reduced recovery evidence, broader mutable state, or speculative shortcuts. It means the least complex **complete and conforming** implementation.

## 6. Forbidden delegated architectural expansion

A delegated implementation agent must stop and report `blocked_architecture` rather than doing any of the following unless the task explicitly fixed that exact change in advance:

- create a new cross-component service, registry, state store, transaction engine, release authority, path authority, or lifecycle owner;
- split, merge, rename, or move a LEGO brick across ownership boundaries;
- add a new public port/interface because it seems cleaner;
- change public schemas, persistence meaning, compatibility policy, or lifecycle state machines;
- change dependency direction;
- modify another product repository;
- interpret provider-specific semantics in Installer;
- broaden a fix from the named owner into adjacent systems;
- weaken safety, trust, rollback, preservation, or validation requirements;
- redesign a prerequisite issue while implementing a dependent issue;
- use KISS to bypass a CUPID quality;
- use CUPID to revise the fixed SOLID design;
- use SOLID to cross or duplicate the fixed LEGO ownership map;
- close an architectural parent issue;
- choose or write `next_step`.

An architectural obstacle is a useful result. Report it with the smallest evidence needed for the organizing model to decide; do not route around it.

## 7. Delegated task packet

No task is Spark-ready until the organizing model supplies all of:

1. parent issue and exact owning LEGO brick;
2. fixed LEGO ownership, lifecycle, replacement, and failure boundary;
3. prerequisite contract/architecture revision;
4. fixed SOLID responsibilities, public interfaces, dependency direction, and substitutions;
5. exact goal and externally observable result;
6. allowed files/directories or explicitly allowed interface surface;
7. fixed input/output types and lifecycle/error semantics;
8. invariants that must remain true;
9. explicit adjacent systems that are out of scope;
10. forbidden new authorities/abstractions;
11. CUPID qualities material to the slice;
12. KISS selection rule among conforming local alternatives;
13. required focused tests/failure injections;
14. stop conditions for unexpected architecture;
15. completion report shape: changed files, tests/evidence, and blocked observations.

If one of these cannot be stated without asking the implementation agent to choose LEGO or SOLID architecture, the slice remains Sol work and must be decomposed further.

## 8. Completion and handback

Spark completion means only one of:

- `completed`: the exact assigned slice and tests are complete;
- `blocked_architecture`: a required architectural decision was not specified or the fixed hierarchy cannot be satisfied;
- `blocked_dependency`: a prerequisite implementation/contract is absent;
- `failed`: the bounded implementation attempt failed with evidence.

The delegated agent does not infer the next task. The organizing model reviews the exact diff and evidence in strict LEGO, SOLID, CUPID, then KISS order; reassesses the parent issue; and alone determines integration and `next_step`.
