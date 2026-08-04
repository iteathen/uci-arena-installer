# Evidence PR #23 hostile review — daemon heartbeat liveness projection

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-evidence-service`  
PR: #23 `agent/project-daemon-heartbeat-staleness`  
Reviewed head: `3991983c397a222871cba70c6232da1a8be5b265`  
Base: PR #22 / `agent/harden-resource-config-after-daemon-timing` at `3e9d9609382a3706aa82db66bca51096ea0dc0b1`  
Formal review: `4850754712`  
Disposition: **changes required; keep draft**

## Confirmed blockers

1. Active spec claims Catalog projection ownership while Catalog is untouched and CLI/API wrappers own separate recalculation.
2. Finite but fractional, unsafe, or out-of-Date-range clocks can violate the declared safe-integer age contract.
3. Permissive `Date.parse` admits noncanonical/platform-dependent timestamp forms into liveness authority.
4. Public stale-threshold override permits consumers to fork the fixed 300000 ms policy.
5. Wrapper ownership samples separate clocks and will duplicate/drift after Catalog integration.
6. Malformed daemon object shape is silently normalized into an ordinary missing heartbeat.
7. Preflight routing omits current CLI and Manager API projection consumers.
8. Direct Catalog, CLI, Manager API, bounded serialization, one-clock, and schema-shape tests are absent; reported test count is stale and only Node 22.16 evidence exists.

## Required correction

- Catalog.status() is sole projection owner with one private/injected clock;
- exact canonical UTC round-trip timestamp admission;
- fixed production threshold and overflow-safe safe-integer age calculation;
- malformed status shape fails visibly rather than becoming ordinary missing;
- wrappers removed and consumers pass through Catalog output;
- spec/preflight reconciled with actual ownership;
- Node >=24.15 direct Catalog, CLI, Manager API, serialization, and boundary tests.

## Queue binding

Jules-control issue #2 was updated to reference hostile review `4850754712` and carry these criteria before launch. PR #23 remains a non-accepted partial and issue #20 remains open.