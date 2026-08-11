# Jules task — complete Evidence issue #20 at the Catalog owner

Do not launch this task while Engine session `sessions/13034909149691235140` is active. When launched, use the official Jules REST API and exact starting branch `agent/project-daemon-heartbeat-staleness` in `iteathen/uci-arena-evidence-service`.

## Objective

Complete the existing partial Evidence PR #23 so `Catalog.status()` itself owns and publishes the typed daemon-heartbeat liveness projection. Do not create a second liveness calculation in CLI or Manager adapters.

## Exact starting state

- repository: `iteathen/uci-arena-evidence-service`
- branch: `agent/project-daemon-heartbeat-staleness`
- expected starting SHA: `3991983c397a222871cba70c6232da1a8be5b265`
- base branch: `agent/harden-resource-config-after-daemon-timing` / PR #22
- source issue: #20
- current PR: #23

Verify the starting branch and SHA before editing. Stop with exact evidence if they differ.

## Read first

- `AGENTS.md`
- repository read-first and spec registry
- `specs/daemon_liveness_v1.yaml`
- `src/daemon_liveness.mjs`
- `src/catalog.mjs`
- `src/cli.mjs`
- `src/manager_api.mjs`
- `test/daemon_liveness.test.mjs`
- PR #23 body and source issue #20

## Required ownership

1. `Catalog.status()` captures one sample time and returns the authoritative daemon projection.
2. Preserve existing `heartbeatAt` compatibility data.
3. Publish exactly:
   - `heartbeat_state`: `missing | fresh | stale`
   - `heartbeat_age_ms`: nonnegative bounded integer or null
4. Use the same sample time for liveness and top-level `sampledAt`.
5. Remove redundant projection calls from CLI and Manager API once Catalog owns the result.
6. Do not infer daemon lock, PID, process, or process-tree authority from heartbeat freshness.
7. Do not change catalog schema, migrations, storage ownership, pruning, job scheduling, or unrelated status fields.

## Tests

Use a real temporary Catalog and an injected/controlled clock or projection seam. Cover:

- direct `catalog.status()` for missing heartbeat;
- 299999 ms fresh and 300000 ms stale boundaries;
- malformed timestamp;
- future timestamp clamped to age zero;
- running, stopped, and failed phases do not change timestamp classification;
- one captured clock controls both `sampledAt` and heartbeat age;
- existing fields and bounded/redacted report behavior remain intact;
- CLI status/init and Manager JSON expose exactly the Catalog-owned projection with no recalculation;
- input objects are not mutated.

## Validation

Run required preflight and, with Node >=24.15.0:

```text
node --test test/daemon_liveness.test.mjs
npm run check
npm run acceptance:idle
```

Record exact commands and output. Automatic CI remains disabled.

## Delivery

Commit only the Catalog-owner integration, removal of redundant wrappers, focused tests, and any required governing spec/preflight correction. Update existing draft PR #23 rather than opening an unrelated PR when possible. Do not merge it. Summarize exact files and any environment limitation.
