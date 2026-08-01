# Exit codes and results

Every launcher returns the installer engine's exit code unchanged.

| Code | Category |
|---:|---|
| 0 | success |
| 2 | invalid invocation |
| 3 | invalid manifest |
| 4 | invalid configuration |
| 5 | failed preflight |
| 6 | unsupported installation state |
| 7 | package validation failure |
| 8 | dependency failure |
| 9 | locked-file failure |
| 10 | staging failure |
| 11 | deployment failure |
| 12 | post-install validation failure |
| 13 | rollback failure |
| 14 | internal installer failure |
| 15 | operator cancelled before mutation |

`arena_installer_result_v1` includes installer and package versions, selected mode and scope, normalized non-secret paths, plan identity, completed/skipped/failed operation, validator results, backup and rollback results, final state, remediation, log path, and exit category. External commands are logged as structured executable and redacted argument arrays.

Human error text must identify the reporting component, expectation, observation, rollback disposition, logs/backups, and next action. “Installation failed” alone is not actionable.
