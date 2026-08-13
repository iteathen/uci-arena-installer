# Recovery

Installation is transactional where the platform permits:

- stage before replacement;
- validate before activation;
- journal before each side effect;
- back up before destructive replacement or migration;
- retain the current working activation until the staged payload is valid;
- atomically rename or switch version roots when supported;
- and restore payload, receipt, configuration, activation, and startup state after failure.

`arena_install_transaction_v1` is durable recovery evidence. An interrupted transaction is detected during inspection and resolved by an explicit recovery plan; staged bytes never become authority merely because they exist.

`RollbackResult` records attempted, completed, failed operation, restored identities, unresolved state, remediation, log location, and backup location. A rollback failure returns the rollback-failed exit category and retains both the original error and recovery error.

Exact-current upgrade refreshes journal receipt and generated-configuration
writes but do not rename immutable component roots or quiesce provider startup.
Their rollback restores the prior receipt and generated files. Burn never plans
rollback restoration for a related bundle it explicitly detected as missing
from cache, so an unavailable stale registration cannot displace the prior
Manager shell during recovery.

Normal uninstall is not secure erase. It durably writes
`arena_install_uninstall_preservation_v1` before removing a receipt, so an
interruption leaves either installed authority or explicit prepared evidence
for diagnosis. A completed record is non-authoritative: it cannot make a
component appear installed.

Purge is separate, previewed, explicitly confirmed, and limited to declared
ownership paths. Destructive purge cannot guarantee rollback, so it records
each removed, already absent, modified-and-preserved, policy-preserved, or
failed operation. A partial failure is reported as `purge_failed`; it is never
collapsed into a generic success. The preservation record remains after purge
as transaction and accountability evidence.
