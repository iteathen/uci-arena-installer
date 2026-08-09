# Upgrade and preservation policy

Clean install, upgrade, repair, reinstall, downgrade, replacement, and uninstall are distinct plans.

- Clean install requires no conflicting authority and creates new receipts.
- Upgrade requires a compatible newer suite, carries feature selection forward, restages selected components, migrates idempotently, and adopts receipts before related-bundle removal.
- The Windows bootstrapper lifecycle adapter claims at most one execute/rollback
  action for each exact related-bundle identity. Duplicate platform detection
  remains logged, while later plan occurrences are explicitly set to no action
  so successful upgrades cannot invoke an already-removed cache entry.
- Repair revalidates retained media, restages selected current components, regenerates installer-owned configuration, and preserves user data.
- Reinstall uses explicit same-version development policy; official releases do not treat different bytes under one version as an upgrade.
- Downgrade is rejected unless an explicit reviewed policy permits it without violating rollback protection.
- Replacement changes scope or product authority only through an explicit migration/uninstall plan.
- Uninstall removes receipt-owned immutable roots, service/task/startup and
  installer-owned integration authority, and receipts. It preserves Manager
  continuity, opening-book and Syzygy assets, Book Forge and Evidence
  workspaces and outputs, profiles, locators, retained media, trust, caches,
  logs, transactions, and all externally owned paths.
- Purge is a separate reviewed plan and confirmation. It can delete only
  bounded, declaratively owned paths from completed uninstall preservation
  evidence; exact managed files whose identity changed are preserved and
  reported.

Ownership classes are application replaceable, user preserved, generated, migrated, cache disposable, installer temporary, external dependency, and unknown policy controlled. Unknown content is never silently deleted or preserved by filename guess.

Removing Book Forge or Evidence Service removes executable and startup
authority without deleting provider workspaces. Repair and reinstall do not
trust retained data merely because it exists; their validators re-establish
identity and compatibility before consumption.
