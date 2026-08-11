# Evidence PR #16 hostile review — transactional prune and recovery

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-evidence-service`  
PR: #16 `agent/transactional-prune-after-show`  
Reviewed head: `7dfc4388f5ce57105e8de515fdcb0526e9298df0`  
Base: PR #15 / `agent/stream-dataset-show-after-export` at `a66fece5f8096622a53784479fe2c8da97fbc4f9`  
Formal review: `4851046968`  
Disposition: **changes required; keep draft**

## Review result

The lifecycle/store/workflow split and same-transaction catalog deletion/count/audit goal are valid improvements. The destructive workflow still lacks exact filesystem/catalog object authority, service-wide writer coordination, durable recovery identity, and truthful committed-failure semantics.

## Confirmed blockers

1. Candidate validation/hash/rename is a path-observation sequence; replacement after hash can restore the wrong object under authoritative rows.
2. Staging and restoration use check-then-ordinary-rename and can overwrite a replacement destination; no atomic no-replace primitive exists.
3. Missing staged source plus any existing destination is treated as restored without size/digest/object validation.
4. Quarantine/ancestor substitution can move an original file outside the captured owned root before escape detection.
5. `PruneStore` opens the catalog through an unadmitted pathname without exact file/workspace/schema/integrity identity.
6. `BEGIN IMMEDIATE` remains open across async hashing/renames and can starve/fail the live daemon or other catalog writers; prune lease does not provide service-wide maintenance authority.
7. Lease interval has no maximum relative to 30-second staleness; future/backward clocks and out-of-Date-range values are mishandled.
8. Malformed lease schemas/nonces/timestamps can still block takeover because only loose object fields are consulted.
9. Long hash/SQLite/filesystem work is not promptly cancellable after lease loss.
10. A committed prune can be hidden behind later heartbeat/timer/audit/marker/release exceptions; operation ID and commit status are lost to the caller.
11. Recovery has no durable frozen manifest binding operation UUID to original path, size, digest, and exact candidate membership.
12. Power-loss/filesystem durability scope is undefined; no fsync/barrier protocol exists.
13. Recovery enumeration/hashing is unbounded and one unknown entry globally blocks all future prune operations.
14. Recovery classification is path-based and can race after the complete scan.
15. Post-commit cleanup deletes by pathname rather than exact staged-object identity.
16. Persisted path/time fields are string-coerced or text-compared without exact storage-schema admission.
17. The production authority guard is regex/string evidence and permits the legacy destructive method to remain.
18. Tests omit the decisive replacement, no-replace, catalog substitution, daemon contention, clock, cancellation, committed-failure, manifest, process-crash, power-loss, and hostile-quarantine cases.
19. Parent PR #15 remains unreviewed.

## Required architectural direction

- exact platform object store for read/hash/no-replace move/restore/delete;
- exact catalog database admission and service-wide maintenance coordination;
- bounded typed lease clocks/cancellation;
- durable operation manifest bound to audit and quarantine;
- typed result preserving committed/pending state plus cleanup diagnostics;
- bounded isolated recovery and explicit crash/durability scope;
- remove the legacy authority and strengthen architecture enforcement.

## Audit consequence

PR #16 is not acceptance evidence. PR #17/#18/#19/#22/#23 remain parent-blocked until this layer and PR #15 ancestry are corrected.