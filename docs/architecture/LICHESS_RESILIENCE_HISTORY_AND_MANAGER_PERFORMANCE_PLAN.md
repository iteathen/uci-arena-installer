# Lichess resilience, history, and Manager performance plan

Status: approved plan; not runtime or release authority

Suite record: [uci-arena-installer#166](https://github.com/iteathen/uci-arena-installer/issues/166)

Owners: Lichess Provider, Manager, and Installer composition

Decision date: 2026-08-23

This plan records why and how the suite will restore Lichess-provider resilience, activate a canonical operational game archive, and remove repeated component hashing from Manager request paths. It deliberately changes no runtime schema, endpoint, launcher, database, or release artifact. Each behavior becomes authoritative only through its issue-owned implementation spec, tests, migration, and release PR.

## 1. Sanitized evidence

- The Windows provider ended abruptly during Lichess game `f4JxmZBM`; Task Scheduler reported `0xC0000409`. No durable provider-owned exit receipt made the exact process instance, exit disposition, or restart eligibility visible after termination.
- The sampled provider diagnostics contained 26 scheduler errors and 6 rate-limit entries, but Manager had no bounded typed incident projection. Important API and recovery failures were therefore easy to miss.
- The provider store contained 646 game summaries, including 10 stale `started` rows. Those games were absent from Manager history, and the summary records did not establish complete replay or trustworthy historical component-version provenance.
- Repeated Manager Overview loads verified roughly 97 MB of installed component payloads, coupling page latency to a full cryptographic rescan.

Credential contents were not read to obtain this evidence and must not be read by any implementation, migration, diagnostic, test, receipt, issue, or projection.

## 2. Design decisions

The plan follows the suite LEGO principles:

1. **One authority per fact.** Provider owns Lichess process recovery, incidents, API cooldowns, canonical games, and archive migration. Manager owns only its read projection and component-attestation lifecycle. Installer owns composition and preservation, not either data plane.
2. **Typed ports instead of storage coupling.** Manager resolves only receipt-verified provider operations. It never opens provider SQLite, runtime files, credential files, or NDJSON logs.
3. **Bounded work and bounded data.** Incident detail, pages, replay length, API backfill batches, transport output, and retry state have explicit limits. Large collections use stable keyset cursors.
4. **Fail closed without hiding state.** Unknown contracts, stale attestation, archive conflicts, ambiguous process ownership, and malformed provider output block authority while exposing a sanitized actionable state.
5. **Incremental projections.** Provider incident/game cursors and Manager filesystem invalidation replace raw-log tailing and repeated whole-dataset rescans.
6. **Compatible rollout.** Additive provider operations land before Manager requires them. Manager continues accepting the current status v3 contract and reports newer features as unavailable.
7. **No invented history.** Historical moves or component versions are never guessed. Incomplete records remain visible as `summary_only`.

## 3. Ownership and work lanes

### 3.1 Provider supervision and incidents

Tracked by:

- [Provider #40: restart unexpected exits and publish durable exit evidence](https://github.com/iteathen/uci-arena-lichess-bot/issues/40)
- [Provider #41: persist a bounded incident ledger and expose typed incident pages](https://github.com/iteathen/uci-arena-lichess-bot/issues/41)

Linux retains one provider-owned systemd authority using `Restart=on-failure` with a 60-second delay. Windows retains one Task Scheduler logon authority and `IgnoreNew`, and adds an indefinite one-minute trigger repetition. The provider's native launcher will atomically publish one bounded exit receipt containing process-instance identity, start and exit timestamps, child exit code, clean/expected versus unexpected disposition, and restart eligibility. It will never contain command lines, paths, credentials, headers, or raw output.

After the storage foundation in Provider #7 lands, the provider will add a durable bounded incident ledger. Repeated incidents aggregate by stable provider-owned code while retaining first/last occurrence, count, severity, active/resolved state, operation, optional HTTP status, retry deadline, game id, and at most 512 characters of sanitized detail. Coverage includes API, scheduler, stream, startup, engine-child, storage, and status-publication failures. Provider-owned Lichess cooldown deadlines are never shortened by recovery.

The planned additive contract exposes `incidents_list`; status v4 adds supervision and active-incident summaries. Exact schemas are intentionally deferred to the provider implementation spec/PR.

Windows design references: [LogonTrigger](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-logontrigger-triggergroup-element), [trigger repetition](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-repetitiontype-complextype), and [IgnoreNew multiple-instance policy](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-multipleinstancespolicy-settingstype-element).

### 3.2 Provider canonical operational game archive

Tracked by [Provider #42](https://github.com/iteathen/uci-arena-lichess-bot/issues/42), after [Provider #6](https://github.com/iteathen/uci-arena-lichess-bot/issues/6) and [Provider #7](https://github.com/iteathen/uci-arena-lichess-bot/issues/7).

The provider will transactionally migrate its store from schema v5 to v6. The existing `games` authority gains immutable completion cursor, initial position, archive-completeness state, provider release identity, and optional neutral engine-component identity. A `game_plies` table, keyed by game id and ply, stores UCI move, before/after FEN, actor, and available clocks.

Provider component id/version/manifest digest are bound exactly. When the public component-manifest binding explicitly supplies them, engine component id/version/manifest digest are also bound. External UCI engines retain executable digest and normalized UCI identities, with component version explicitly absent.

Authoritative streamed plies append or reconcile idempotently. Takebacks may rewrite only the nonterminal current line; a terminal archive is immutable. Existing summary rows backfill through the provider API coordinator in batches of at most 300 ids, never blocking move submission or challenge readiness. Historical PGN/SAN passes through a bounded provider-owned parser and canonical legality reconstruction. Conflicts or unavailable records remain `summary_only`, and historical component versions are never guessed. The official Lichess export-by-ids operation supports up to 300 ids and move/clock export: [Lichess OpenAPI specification](https://raw.githubusercontent.com/lichess-org/api/master/doc/specs/tags/games/api-games-export-_ids.yaml).

Games are retained indefinitely in v1. The provider exposes total/incomplete counts and storage size. Pruning and training export remain separate policy work.

The planned typed operations are:

- `games_list`, scoped to `live` or `history`, with stable keyset cursors and pages capped at 100;
- `game_get`, returning bounded replay/provenance for at most 2,048 plies.

Exact storage and operation schemas are intentionally deferred to the provider implementation spec/PR.

### 3.3 Manager provider projections

Tracked by:

- [Manager #125: project incidents and recovery state](https://github.com/iteathen/uci-arena-manager/issues/125)
- [Manager #126: integrate provider-owned games](https://github.com/iteathen/uci-arena-manager/issues/126)

Manager will add generic read-only routes for provider status, incidents, games, and game detail. They resolve only receipt-verified declared operations and validate bounded project-owned schemas. Manager will show active critical/error incidents as persistent Overview and Bots alerts with retry/recovery context and a navigation badge. Updates use incident cursors and identity deduplication, not raw-log tailing.

Existing Live Games and Game History surfaces will merge Manager-owned local games with provider-owned Lichess projections using a source filter and badge. Replay is enabled only for archive-complete provider records. `summary_only` records show incompleteness, exact available provenance, and a Lichess link without fabricated positions or versions.

Generic bounded pagination remains owned by [Manager #106](https://github.com/iteathen/uci-arena-manager/issues/106), and the backend Overview operational summary remains owned by [Manager #110](https://github.com/iteathen/uci-arena-manager/issues/110).

### 3.4 Manager component-attestation lifecycle

Tracked by [Manager #127](https://github.com/iteathen/uci-arena-manager/issues/127).

One Manager-owned registry will perform full cryptographic verification asynchronously once after each Manager start. `/components` returns `checking`, `verified`, or `failed` immediately and never hashes payloads on the HTTP request thread. Attestation authority is cached only in memory for that process.

Platform filesystem notifications invalidate affected components; watcher overflow invalidates all. Invocation remains blocked while verification is pending, stale, or failed. Repeated Overview loads consume the cached projection instead of rescanning payloads.

## 4. Dependency and rollout order

1. Merge the coordinated documentation PRs. They record the target but activate no runtime behavior.
2. Complete Provider #6 atomic activation/abnormal termination and Provider #7 SQLite hardening.
3. Implement provider restart/exit evidence and incident contracts (#40 and #41).
4. Implement provider v5-to-v6 archive migration, live reconciliation, and bounded historical backfill (#42).
5. Implement compatible Manager incident and game projections (#125 and #126). Older providers remain usable with the new features unavailable.
6. Implement and measure Manager asynchronous component verification (#127).
7. Activate compatible component releases only after cross-repository substitution and acceptance gates pass.

[Provider #31](https://github.com/iteathen/uci-arena-lichess-bot/issues/31) remains closed; the new contract exposes its provider-owned rate-limit state. [Manager #117](https://github.com/iteathen/uci-arena-manager/issues/117) remains closed unless its earlier PID-reuse signature is independently reproduced.

## 5. Acceptance gates

- Unexpected Windows/Linux exits restart within two minutes, create no duplicate authority, and remain visible after recovery; clean shutdown neither emits a crash incident nor loops.
- HTTP 400/429, cooldown recovery, scheduler faults, stream failures, malformed error bodies, startup failures, engine-child failures, storage failures, and status-publication failures yield bounded sanitized incidents.
- Crash, reconnect, takeback, and duplicate-stream fixtures produce one canonical game and immutable terminal replay.
- Every known historical summary is attempted in bounded batches; conflicts, unavailable games, and unknown historical versions remain visibly incomplete.
- With 646 or more provider games, every row is cursor-reachable and every complete replay reconstructs from provider-authored FEN snapshots.
- Fake-provider substitution uses different executable layouts and command prefixes without Manager behavior changes.
- Overview renders while deliberately delayed verification of more than 100 MB reports `checking`; repeated component requests perform no hashing.
- File mutation or watcher overflow invalidates cached attestation and prevents invocation until exact revalidation completes.
- No credential content appears in a test, diagnostic, receipt, issue, document, incident, archive projection, or Manager response.

## 6. Non-goals

- Training export, Restaurant source integration, or CCR materialization
- Automatic game pruning or a retention/deletion policy
- Manager-side provider persistence or direct provider-store access
- Raw-log tailing as the incident integration
- Runtime JSON schema changes in the documentation phase
- Reopening closed issues without new evidence

The portable PGN/JSONL training bundle in the complete-suite architecture remains a reservation only. Any future activation requires separate policy for eligibility, licensing, privacy, retention, deletion, and redistribution.
