# Installer lifecycle

The orchestrator owns one stable sequence:

1. Parse invocation options.
2. Load configuration and policy.
3. Load and validate catalog and component manifests.
4. Inspect the environment.
5. Detect existing installation state.
6. Build and report an explicit plan.
7. Preflight release-media retention, including every local payload path,
   declared size, and digest.
8. Stage immutable payloads.
9. Validate staged payloads.
10. Back up replaceable state when policy requires it.
11. Apply the ordered plan.
12. Run post-install validators.
13. Commit receipts, activation, and result.
14. Roll back after a critical failure.
15. Emit a structured result and stable exit category.

The planner decides; execution adapters perform. Major decisions may not be discovered during deployment. Dry-run validates the same manifest, state, policy, capacity, and operation graph without applying mutations.

Every side effect is journaled before execution. Provider startup changes occur before an immutable root is replaced, and prior startup/configuration state is restored during rollback. Original failure and rollback failure are always reported separately.

After replacement receipts, provider state, and the Manager restart have
committed, the deployment adapter performs bounded superseded-root maintenance.
It examines only siblings of the active root for a currently receipted component
and removes a sibling only when its manifest and complete payload still match
Installer ownership evidence. Modified, invalid, linked, and unrecognized roots
are preserved and reported. This maintenance is idempotent and cannot roll back
the newly committed installation; a later repair or upgrade retries it.

Release-media policy is decided before any process, component, receipt,
workspace, or service mutation. Retention revalidates identity at the point of
copy to defend against media changes between preflight and commit.

## Removal lifecycle

Uninstall and purge are separate lifecycle modes:

1. `uninstall-plan` selects installed receipts, verifies dependency order and
   bundle ownership, and records the exact receipt digest, active root, startup
   cleanup requirement, and future preservation-record path. It performs no
   mutation.
2. Confirmed `uninstall` rejects stale plans, writes preservation evidence,
   removes receipt-owned startup and executable authority, then removes the
   receipt. Managed-copy books and every declared persistent path remain.
3. `purge-plan` reads the latest non-authoritative preservation evidence. It
   rejects active installations, root-equality (including nested configured
   roots), unbounded paths, unknown ownership, symbolic-link or Windows reparse
   traversal, protected lifecycle overlap, and recursive ownership that would
   engulf an external path. It then separates deletable operations from paths
   preserved by policy.
4. Confirmed `purge` revalidates the evidence digest and path bounds before
   every deletion. Exact-identity files are deleted only while their recorded
   size and SHA-256 still match. Modified files are retained and reported.

Retained state never establishes installation authority. A reinstall must
revalidate retained workspaces, assets, locators, profiles, release media, and
trust state before use. Purge is deliberately not modeled as rollback-capable:
its reviewed plan, per-operation outcome, partial failures, and preserved files
remain in the preservation record.
