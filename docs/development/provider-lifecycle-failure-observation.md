# Provider lifecycle failure observation

Status: implemented candidate; independent review required before deployment.

This repair retains a failed `arena_provider_lifecycle_result_v1` before Apply
throws. It does not alter provider operations, replacement ordering, compensation
intent, or ready/safe-inactive admission. `provider_lifecycle_failures` separates
forward and compensation observations; existing receipts keep the same schema.

Machine JSON uses stdout, with stderr discarded by the platform. Lifecycle
capture admits at most 65,536 actual stdout bytes and rejects dropped or excess
bytes and mixed output. Numeric exit facts classify invalid/no-frame failures;
an exact valid neutral frame retains its existing disposition authority. A final frame without a newline
is joined and drained before parsing. Other `run_json` callers keep their prior
output capacity unless they supply a bound. Ordinary NativeProcess callers keep
merged output by default.

`ProcessCommandFailure` accepts closed categories and numeric facts only. Unknown
adapter exception text is explicitly redacted. No unlabelled child bytes, hashes,
argv, private configuration or store contents are copied to diagnostics. A
provider-owned bounded neutral reason remains opaque under its existing
secrets-forbidden contract; a readable producer exception must use that contract.

Native fixtures reproduce the historical missing failed journal, empty adapter
reason and stderr/JSON mixing. Canary, malformed/mixed output, exact limit,
oversized tail, no-newline, nonzero exit, and forward/compensation tests qualify
the repair. These are isolated synthetic providers. They do not establish the
underlying cause of the retained failed .53 installation or perform an installed
provider operation.

A deterministic Windows fixture holds the actual transaction journal without
DELETE sharing after the provider invocation starts. Both diagnostic and final
journal activation then fail. The returned error preserves the original neutral
cause and compensation failures, adds controlled persistence notices, and says
the final journal state was not persisted. The prior journal remains unchanged;
this result does not claim a durable rollback record.
