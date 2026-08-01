# Installer components

| Brick | Owns | Does not own |
|---|---|---|
| Launcher | Scope choice, child location, argument and exit-code forwarding | Extraction, policy, validation, rollback |
| Orchestrator | Stable lifecycle and coordination | File lists or platform commands |
| Package source | Offline/online transport and digest-verified bytes | Install policy |
| Windows child-bundle source | Versioned envelope, exact embedded bundle digests, temporary child materialization and cleanup | Component extraction, install policy, rollback |
| Manifest loader | Schemas, identities, mappings, entrypoints, prerequisites | Mutation |
| Configuration loader | Explicit roots, feature choices, precedence | Upgrade decisions |
| Environment detector | Platform, privilege, volumes, runtimes, locks, ports | Repair policy |
| State detector | Evidence about existing and interrupted state | Mode selection |
| Planner | Mode, policy, capacity, ordered typed operations | Filesystem/process mutation |
| Staging manager | Isolated stage and cleanup | Activation |
| Deployment adapter | Atomic activation and replaceable-file operations | Preservation policy |
| Dependency adapter | Typed platform/tool operations | Arbitrary shell execution |
| Configuration migrator | Versioned idempotent migrations and receipts | Package acquisition |
| Preservation manager | Ownership classes and preserved-data evidence | Deployment mechanics |
| Validators | Independent named observations and remediation | Orchestration |
| Backup manager | Backup identity, creation, retention | Rollback decision |
| Rollback manager | Prior-state restoration and exact result | Hiding the original failure |
| Logging provider | Structured, correlated, redacted logs | Policy |
| Result reporter | Bounded result and stable exit category | Mutation |

An interface is justified by ownership, change pressure, platform behavior, recovery semantics, or independent testing. Basic language and filesystem operations remain simple implementation details inside their owner.
