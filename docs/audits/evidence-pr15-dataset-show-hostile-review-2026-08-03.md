# Evidence PR #15 hostile review — streamed dataset show

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-evidence-service`  
PR: #15 `agent/stream-dataset-show-after-export`  
Reviewed head: `a66fece5f8096622a53784479fe2c8da97fbc4f9`  
Base: PR #14 / `agent/stream-dataset-export-after-cli` at `fbb1a9aa8d0ae1a2ad14be6a00d3bb229285a173`  
Formal review: `4851061354`  
Disposition: **changes required; keep draft**

## Review result

One validated serializer plus spool-before-output is the correct high-level design and bounds heap memory. The current workflow does not retain the exact validated file object or robustly own writable/source/cleanup lifecycle.

## Confirmed blockers

1. The validated spool pathname is reopened after export; replacement can be streamed as validated output.
2. Path-based cleanup can delete a replacement object.
3. Exports/spool directory is mutable unadmitted pathname authority and can be redirected by symlink/junction/reparse substitution.
4. Final rename can overwrite a concurrently created final object; no no-replace publication exists.
5. Cleanup failure is silently swallowed, potentially retaining a complete sensitive dataset while reporting success.
6. Writable close/destroy/already-ended/callback omission states can hang or lose failure.
7. Late writable errors can become unhandled after per-chunk listeners are removed.
8. Source stream destruction/closure before cleanup is implicit rather than proven, especially on Windows open-handle semantics.
9. Missing dataset still creates workspace directory state without an explicit contract.
10. Spool disk/work/cancellation is unbounded even though heap memory is bounded.
11. Successful delivery result does not bind exact bytes/digest to the validated spool.
12. Tests omit object replacement, root substitution, no-replace collision, cleanup failure, writable terminal races, exact output identity, and Node 24.15 Windows/Linux behavior.
13. Parent PR #14 remains unreviewed.

## Required correction

- stream from the exact validated descriptor/handle and delete only that object;
- exact exports-root admission and no-replace operation-relative creation;
- typed cleanup status and bounded failure causality;
- complete writable/source terminal-state ownership;
- exact successful byte/digest delivery evidence;
- free-space/cancellation policy;
- review/correct PR #14.

## Audit consequence

PR #15 is not acceptance evidence. PR #16 and every later Evidence branch remain parent-blocked.