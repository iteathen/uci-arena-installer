# Evidence PR #14 hostile review — streamed dataset export

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-evidence-service`  
PR: #14 `agent/stream-dataset-export-after-cli`  
Reviewed head: `fbb1a9aa8d0ae1a2ad14be6a00d3bb229285a173`  
Base: PR #13 / `agent/strict-cli-contract-after-system-sanity` at `7b5e7f90476e17e5157f4a782a27122aa210a90b`  
Formal review: `4851124960`  
Disposition: **changes required; reconstruction required; keep draft**

## Review result

The lazy SQLite iterator and incremental JSON/JSONL serializer are useful ownership slices. This head does not establish exact Catalog identity, strict stored evidence admission, bounded snapshot/resource behavior, race-safe publication, complete durability, cleanup causality, or a success result bound to the exact published bytes.

## Confirmed blockers

1. `DatasetReadStore` reopens a mutable pathname and does not bind the exact database object to the already-open Catalog, workspace, Catalog application identity, `metadata.schema_version`, expected schema/index contract, or a bounded integrity policy.
2. Dataset admission validates only the observation count. It does not require exact dataset schema/state/IDs/timestamps or typed and bounded target/authority values.
3. Observation mapping hard-codes the envelope schema and parses JSON without validating dataset binding, IDs, timestamp, adapter/source schema, SHA-256, or exact typed/bounded JSON field contracts.
4. Stored JSON and per-observation serialization are unbounded. Pretty JSON multiplies allocation through stringify/split/map/join; “one observation at a time” is not an exact memory bound.
5. The live SQLite read transaction remains open through parent creation, temporary-file creation, all serialization, file sync, and close. Long exports can pin WAL checkpoints and permit unbounded WAL/disk growth.
6. Catalog/WAL/SHM/journal protection is lexical only and is bypassable through case aliases, symlinked/junction/reparse ancestors, alternate mount paths, and other physical aliases.
7. Parent-directory identity is not established or revalidated. Mutable ancestor substitution can redirect both temporary creation and final publication.
8. Final `rename` is overwrite-capable on supporting platforms. The specification and test intentionally accept replacement of a prior destination rather than requiring provider-neutral no-replace publication.
9. The temporary object is closed and later trusted by pathname. It can be replaced before rename; cleanup can delete a replacement object the exporter never created.
10. Only the temporary file is synced. Parent/destination directory durability and exact crash-survival scope are absent.
11. Handle-close, iterator-close, rollback, and temporary-removal failures are swallowed or can replace the primary failure. CLI `readStore.close()` and outer `catalog.close()` can also replace export failure.
12. Success returns dataset ID, format, path, and count only. It does not bind exact bytes, digest, final object identity, or prove the published object is the serialized object.
13. Path bytes/depth, observation count, per-value and cumulative bytes, JSON complexity, free space, elapsed time, cancellation, write/work count, and diagnostics are unbounded.
14. Tests cover the intended implementation shape but omit decisive production races: exact Catalog substitution, valid-JSON/wrong-schema rows, oversized/deep values, concurrent writer/WAL growth, physical aliases/reparse ancestors, temp replacement, destination appearance before publish, no-replace behavior, sync/cleanup/commit failures, cancellation/resource limits, final digest/object binding, and native Windows/Linux behavior.

## Required reconstruction boundary

- Keep SQLite schema ownership in one persistence adapter, but require exact Catalog/workspace/schema/integrity identity and strict typed row admission.
- Bound the live snapshot lifetime; use a persistence-owned bounded/cancellable stream or exact spool/snapshot object that releases the active Catalog promptly.
- Give publication one project-owned provider-neutral port owning exact parent/destination/temp identity, no-replace semantics, directory/file durability, and cleanup failure causality.
- Compute and return exact output bytes and digest from the same object that becomes authoritative.
- Add exact byte, path, observation, work, time, cancellation, free-space, diagnostic, and cleanup bounds.
- Attack the real production seams and race windows in tests, including native Windows and Linux behavior under Node 24.15.

## Audit consequence

PR #14 is not acceptance evidence. PR #15 and every later Evidence branch remain parent-blocked. PR #14 is also independently blocked by PR #13 and PR #10.