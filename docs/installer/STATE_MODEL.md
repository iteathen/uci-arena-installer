# Installation state model

The state detector gathers evidence and classifies the target as one of:

- absent;
- current valid;
- older valid;
- newer valid;
- unsupported;
- partial;
- corrupt;
- interrupted;
- staged but uncommitted;
- locked;
- development workspace; or
- repair required.

Evidence includes scope marker, MSI or package ownership, component receipts, active roots, transaction journals, staging roots, manifest digests, service/startup registrations, generated configuration, and health validation. A directory existing is never sufficient evidence.

The planner maps state plus options and policy to clean install, upgrade, repair, reinstall, downgrade, replacement, or uninstall. Unsupported mappings fail before destructive work.

Personal and All-users installations are mutually exclusive Manager authorities. Detection preserves the installed scope for maintenance and requires an explicit uninstall or migration decision before changing scope.
