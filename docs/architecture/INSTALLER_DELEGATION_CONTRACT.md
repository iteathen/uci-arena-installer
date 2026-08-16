# Installer Delegation Contract

Status: active execution policy for the Installer hardening program while lower-capability implementation agents are used.  
Applies to: the architecture and issue set described by `INSTALLER_HARDENING_ARCHITECTURE.md`.

## 1. Principle

Delegation happens **after architecture**, never instead of architecture.

The organizing Sol-class model owns LEGO and architectural SOLID. A delegated Spark-class model may apply CUPID and KISS only inside a boundary that has already been fixed.

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
  -> CUPID implementation quality
  -> KISS implementation simplicity
  -> idiomatic local code
  -> focused tests/evidence for the assigned slice
                     |
                     v
Sol-class organizing model
  -> architectural review/reassessment
  -> integration decision
  -> parent-issue status
  -> next implementation slice / next_step
```

## 2. Sol-owned decisions

The organizing model decides before any delegated implementation begins:

- what the meaningful LEGO bricks are;
- which repository/component owns each responsibility and lifecycle;
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

## 3. Spark-owned decisions

Inside the already-fixed brick and interface, Spark may decide local implementation details using CUPID and KISS:

- choose straightforward private helper structure;
- use idiomatic language/library constructs;
- reduce incidental complexity and duplication within the assigned owner;
- keep control flow comprehensible and predictable;
- keep private names, functions, and data structures easy to use correctly;
- keep error handling explicit and unsurprising;
- implement tests and fixtures required by the assigned contract;
- make a local implementation smaller/simpler when externally observable behavior and fixed ownership remain unchanged.

Spark should prefer the simplest implementation that completely satisfies the assigned contract. It should not add abstraction for hypothetical future variation.

## 4. Forbidden delegated architectural expansion

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
- close an architectural parent issue;
- choose or write `next_step`.

An architectural obstacle is a useful result. Report it with the smallest evidence needed for the organizing model to decide; do not route around it.

## 5. Delegated task packet

No task is Spark-ready until the organizing model supplies all of:

1. parent issue and exact owning LEGO brick;
2. prerequisite contract/architecture revision;
3. exact goal and externally observable result;
4. allowed files/directories or explicitly allowed interface surface;
5. fixed input/output types and lifecycle/error semantics;
6. invariants that must remain true;
7. explicit adjacent systems that are out of scope;
8. forbidden new authorities/abstractions;
9. required focused tests/failure injections;
10. stop conditions for unexpected architecture;
11. completion report shape: changed files, tests/evidence, and blocked observations.

If one of these cannot be stated without asking the implementation agent to choose architecture, the slice remains Sol work and must be decomposed further.

## 6. Completion and handback

Spark completion means only one of:

- `completed`: the exact assigned slice and tests are complete;
- `blocked_architecture`: a required architectural decision was not specified;
- `blocked_dependency`: a prerequisite implementation/contract is absent;
- `failed`: the bounded implementation attempt failed with evidence.

The delegated agent does not infer the next task. The organizing model reviews the exact diff and evidence against LEGO/SOLID architecture, reassesses the parent issue, and alone determines integration and `next_step`.
