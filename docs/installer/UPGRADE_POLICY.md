# Upgrade and preservation policy

Clean install, upgrade, repair, reinstall, downgrade, replacement, and uninstall are distinct plans.

- Clean install requires no conflicting authority and creates new receipts.
- Upgrade requires a compatible newer suite, carries feature selection forward, restages selected components, migrates idempotently, and adopts receipts before related-bundle removal.
- Repair revalidates retained media, restages selected current components, regenerates installer-owned configuration, and preserves user data.
- Reinstall uses explicit same-version development policy; official releases do not treat different bytes under one version as an upgrade.
- Downgrade is rejected unless an explicit reviewed policy permits it without violating rollback protection.
- Replacement changes scope or product authority only through an explicit migration/uninstall plan.
- Uninstall removes receipt-owned immutable roots and startup authority while preserving declared operator state unless purge is separately previewed and confirmed.

Ownership classes are application replaceable, user preserved, generated, migrated, cache disposable, installer temporary, external dependency, and unknown policy controlled. Unknown content is never silently deleted or preserved by filename guess.
