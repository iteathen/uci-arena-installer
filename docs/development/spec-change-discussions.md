# Specification change discussions

Updated: 2026-10-11. A proposal is not accepted specification authority merely
because code exists. This record keeps operator decisions separate from source
qualification, installed acceptance, and future proposals.

| ID | Status | Observed gap and owner | Discussion outcome |
| --- | --- | --- | --- |
| SP-001 | Accepted by user; source merged in PR210; unsigned 0.3.60 upgrade accepted on prepared E root | Complete 0.3.59 installed on E:, but inherited broad write permissions on its data root prevented Manager authentication startup. Installer owns fresh per-user root preparation before state publication. | The user selected **refuse installation and explain explicit folder preparation or a fresh location**. Fresh roots receive restricted permissions before visibility; unsafe existing roots are inspected and refused. No automatic or separate permission-adoption feature is authorized. Manager key admission stays unchanged. |
| SP-001 publication clarification | Accepted ownership clarification; source merged in PR210; unsigned 0.3.60 upgrade accepted on prepared E root | Native RED reproduced a valid signed/TUF `plan` publishing rollback state into a broadly writable root before confirmation. A confirmation-only guard cannot protect the ordinary plan-to-confirm flow. | Transfer the resolved per-user root explicitly to the actual TUF state-publication owner. Verify signatures, expiry, and metadata linkage before root creation; preserve rollback checks. Reuse exact no-reparse read/publication owners and root containment. A TUF plan may publish anti-rollback security state; a nonpublishing query need not create a root. This is not permission adoption or weaker TUF validation. |
| SP-002 | Accepted clarification, implemented and documented | Managed engine profiles needed consumer-owned original-clock option bindings while preserving baseline options and engine identity. | Vector PR49, Manager PR146, and Evidence PR58 implement the explicit local diagnostic/competition-clock seam. Original clocks are not inferred from remaining time or timing profiles. Engine search, model, and qualified policy remain unchanged. |
| SP-003 | Verified operational documentation update | Installed readiness and targeted regressions must remain distinct from general strength evidence. | The current signed Vector 0.1.4 wrapper preserves the qualified 0.1.3 runtime/source 31d. Qualification remains bounded to its documented 180000+0 control and campaign. Keep failed trials failed and distinguish the 80 mixed-game campaign from 32 selected-policy games. |
| SP-004 | Accepted by user; governance reconciliation under review | Historical Manager124 prohibited clock transit too broadly, conflicting with reviewed 0.3.29 probe/competition clock bindings. | The user selected **allow forwarding, keep timing calculations with their existing owners**. Manager forwards actual original/remaining UCI clocks and increment unchanged through declared bindings; it does not calculate timing-policy strategy/allocation or qualification. Vector/current Evidence runtime-v2 and between-game admission remain authoritative. |
| YAML syntax maintenance | Formatting repair qualified; no new policy | Strict YAML parsing found existing errors in five Installer spec documents; the current registry/schema checker does not parse YAML syntax. | Preserved original failures and literal before/after documents. Quote/fold existing prose and restore explicit json_contract.rules and prerequisites.required lists plus the misplaced forbidden-effects sibling. Every existing rule word/order is preserved; no executable consumer uses those malformed key paths. All 39 YAML documents across the three reconciled producers now parse. This introduces no runtime behavior or intended semantic rule. |

The retained operational record identifies committed Complete 0.3.59 transaction
17917516509559016, eight correct component versions and 990 verified inventoried
files, Manager doctor/restart success, two targeted Vector regressions, and a
preserved Bot archive with the Bot stopped. The E root's permission preparation
was an explicit operator repair. It is not evidence that 0.3.59 automatically
prepares secure custom roots, nor a new engine-strength qualification.

Complete 0.3.60 requires Installer runtime 0.2.3 for SP-001. Its eight component
versions and signed inputs remain unchanged. Source qualification passed: Windows
native 228 passes with four existing privilege skips; JavaScript 166 passes with
six declared skips; focused Windows/E permission coverage 291 assertions in eight
cases with no skips. Independent source review passed and PR210 merged as
`f332669a72401a972526c6c0b3ddfb7165785b58`. The reviewed unsigned development
0.3.60 upgrade joined with exit 0 on the already-prepared E installation. Eight
components/990 files and runtime 0.2.3/84 files matched; Manager cold-host doctor
and retained engine IDs passed, while Bot remained stopped. Actual existing-root
upgrade acceptance is separate from the native fresh-root/refusal fixture proof
and grants no new engine-strength or live-play authority. See
[retained acceptance](preserving-fresh-install.md#verified-e-installation-and-0360-upgrade).

Future proposals remain open for additions. Each entry should identify the
concrete gap, proposed semantic owner, compatibility effect, qualification limit,
and user discussion outcome before becoming accepted behavior.
