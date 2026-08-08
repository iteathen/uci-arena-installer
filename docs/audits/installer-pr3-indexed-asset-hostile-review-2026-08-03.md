# Installer PR #3 hostile review — indexed asset result bound

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-installer`  
PR: #3 `agent/bound-indexed-asset-results`  
Reviewed head: `05776766064a014836650d46179eadffb73fed2a`  
Base: stale `agent/release-assembly` SHA `c43cdd659bab28bc96bc81d407ab4e7e10c9ac33`  
Formal review: `4850804908`  
Disposition: **changes required; stale ancestry; keep draft**

## Confirmed blockers

1. Windows `TOP N` has no ORDER BY and Linux caps provider enumeration before shared sorting, so the returned capped subset is not deterministic.
2. Duplicate/case-variant rows consume raw capacity before final deduplication, underfilling the result and starving later categories.
3. `truncated` is asserted when exactly N rows are observed and when the global vector is merely full; no omitted candidate is proved.
4. Sequential opening-book → Syzygy → timing-profile allocation introduces starvation/product priority without a governing contract, and platform query arrays duplicate the supposed port-owned order.
5. Optional OLE DB command-timeout rejection is not inspected, and synchronous Execute can exceed the global deadline.
6. Timed-out/failed/unavailable results retain partial candidates without typed authority semantics.
7. Linux collapses process lifecycle, cleanup, timeout, and provider execution failures into ordinary locate unavailability and depends on rejected/stale NativeProcess ancestry.
8. Windows lexical path equality does not normalize case-insensitive physical path identity for deduplication.
9. Tests cover helper arithmetic/finalization only, not deterministic provider selection, duplicate carry-forward, exact truncation, cross-kind policy, timeout enforcement, partial-result authority, or real providers.
10. The PR is now `mergeable=false`; its zero-behind claim is stale after PR #2 advanced.

## Required correction

- deterministic provider-neutral bounded top-K/order before cap;
- dedup during selection and truthful capacity/truncation status;
- explicit cross-kind fairness/priority contract owned once by the port;
- enforceable Windows/Linux deadline behavior;
- typed partial-result authority/failure semantics;
- real locate/plocate and Windows Search tests;
- rebuild on current PR #2 ancestry.

## Audit consequence

PR #3 is not acceptance evidence. The global working-set goal remains valid, but the current result cannot be used for package/discovery soundness.