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

Normal uninstall is not secure erase. Purge is separate, previewed, explicit, and limited to declared paths.
