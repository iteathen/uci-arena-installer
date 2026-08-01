# Installer lifecycle

The orchestrator owns one stable sequence:

1. Parse invocation options.
2. Load configuration and policy.
3. Load and validate catalog and component manifests.
4. Inspect the environment.
5. Detect existing installation state.
6. Build and report an explicit plan.
7. Stage immutable payloads.
8. Validate staged payloads.
9. Back up replaceable state when policy requires it.
10. Apply the ordered plan.
11. Run post-install validators.
12. Commit receipts, activation, and result.
13. Roll back after a critical failure.
14. Emit a structured result and stable exit category.

The planner decides; execution adapters perform. Major decisions may not be discovered during deployment. Dry-run validates the same manifest, state, policy, capacity, and operation graph without applying mutations.

Every side effect is journaled before execution. Provider startup changes occur before an immutable root is replaced, and prior startup/configuration state is restored during rollback. Original failure and rollback failure are always reported separately.
