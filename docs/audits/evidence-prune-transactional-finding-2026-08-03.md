# Evidence Service Transactional Prune Finding — 2026-08-03

## Ledger entry

| Question ID | Question | Status | Evidence / disposition |
|---|---|---|---|
| Q-EVIDENCE-004 | Can confirmed prune fail or crash at any boundary without leaving authoritative catalog rows pointing at missing raw evidence, deleting an external/ambiguous file, or racing another recovery operation? | contradiction | The legacy `Catalog.prune()` deleted files inside an open SQLite transaction, trusted stored paths, materialized all candidates, and inserted the audit only after the destructive transaction. Corrected on stacked Evidence draft PR #16; local Node 24 and destructive process-interruption acceptance remain required before classification can become `answered`. |

## EVIDENCE-PRUNE-041 — non-transactional two-resource deletion

- Category: retention / database-filesystem consistency / crash recovery
- Severity: critical
- Status: corrected on branch, pending validation
- Owner: Evidence Service prune lifecycle
- Original contradiction:
  - candidate artifacts were materialized with SQLite `.all()`;
  - file deletion occurred before the database transaction committed;
  - a later SQL or audit failure could roll catalog rows back after files were
    irreversibly removed;
  - stored paths were not physically bounded to the artifact root;
  - symlink, size, and SHA-256 identity were not revalidated;
  - audit insertion occurred after the destructive transaction;
  - no crash-recoverable staging or operation serialization existed.

## Correction

Evidence Service draft PR #16, branch
`agent/transactional-prune-after-show`, adds three ownership-separated bricks:

1. `PruneOperation`
   - owns one catalog-backed PID/nonce lease;
   - maintains a separate heartbeat;
   - serializes recovery, dry run, mutation, rollback, cleanup, and audit
     finalization;
   - preserves workflow, heartbeat, timer, and release failures.
2. `PruneStore`
   - owns SQLite lease metadata and exact-owner release;
   - uses aggregate dry-run queries;
   - freezes destructive candidates in a connection-local temporary table
     under `BEGIN IMMEDIATE`;
   - streams candidates;
   - deletes observation/artifact rows, reconciles dataset counts, and inserts
     the confirmed audit in one transaction.
3. `PruneService`
   - validates lexical and physical ownership, regular-file status, size, and
     canonical SHA-256 identity;
   - atomically stages files under an operation UUID in
     `artifacts/.prune-quarantine`;
   - revalidates identity after rename and before row deletion;
   - restores valid staged files after rollback;
   - never restores an altered file beneath an authoritative catalog path;
   - classifies complete recovery state before mutation;
   - deletes rowless quarantine files only with matching confirmed pending audit
     authority;
   - leaves conservative recoverable markers after post-commit cleanup failure.

The production CLI now composes `PruneOperation` and `PruneStore`. A mandatory
source guard fails any runtime call to the legacy `Catalog.prune()` path, any
prune SQL outside `PruneStore`, direct CLI import of the inner workflow, or
reintroduction of the rejected filesystem lock.

## Pull-request dependency

Evidence stack order is now:

1. PR #10 — broad package/shared-boundary hardening.
2. PR #13 — exact CLI grammar.
3. PR #14 — streamed dataset export.
4. PR #15 — streamed dataset display.
5. PR #16 — transactional crash-recoverable prune.

PR #16 is a draft and targets the PR #15 branch. Nothing from this repair was
committed directly to `main`.

## Required validation before closure

- `npm run check` on Node 24.15 or newer.
- `npm run acceptance:idle`.
- Two-process concurrent prune exclusion.
- Forced termination in each interval:
  - before staging;
  - after one or more staged renames but before SQLite commit;
  - after SQLite commit but before quarantine deletion;
  - after quarantine deletion but before audit finalization;
  - after audit finalization but before marker removal.
- Windows junction/reparse and Linux symlink escape fixtures.
- Large synthetic candidate set with bounded JavaScript memory.
- Digest/size/path mutation between initial validation and staged revalidation.
- Package-installed prune and next-start recovery.
- Local removal of the now-dead monolithic `Catalog.prune()` definition after
  branch rebase, while retaining the authority guard.

Until these checks pass, the final state remains `contradiction` with a pending
correction, not `answered`.
