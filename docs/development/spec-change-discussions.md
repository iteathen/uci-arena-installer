# Specification change discussions

Updated: 2026-10-11. A proposal is not accepted specification authority merely
because code exists. This record keeps operator decisions separate from source
qualification, installed acceptance, and future proposals.

| ID | Status | Observed gap and owner | Discussion outcome |
| --- | --- | --- | --- |
| SP-001 | Accepted by user; source qualification passed, review/packaging pending | Complete 0.3.59 installed on E:, but inherited broad write permissions on its data root prevented Manager authentication startup. Installer owns fresh per-user root preparation before state publication. | The user selected **refuse installation and explain explicit folder preparation or a fresh location**. Fresh roots receive restricted permissions before visibility; unsafe existing roots are inspected and refused. No automatic or separate permission-adoption feature is authorized. Manager key admission stays unchanged. |
| SP-001 publication clarification | Accepted ownership clarification; source qualification passed, review/packaging pending | Native RED reproduced a valid signed/TUF `plan` publishing rollback state into a broadly writable root before confirmation. A confirmation-only guard cannot protect the ordinary plan-to-confirm flow. | Transfer the resolved per-user root explicitly to the actual TUF state-publication owner. Verify signatures, expiry, and metadata linkage before root creation; preserve rollback checks. Reuse exact no-reparse read/publication owners and root containment. A TUF plan may publish anti-rollback security state; a nonpublishing query need not create a root. This is not permission adoption or weaker TUF validation. |
| SP-002 | Accepted clarification, implemented and documented | Managed engine profiles needed consumer-owned original-clock option bindings while preserving baseline options and engine identity. | Vector PR49, Manager PR146, and Evidence PR58 implement the explicit local diagnostic/competition-clock seam. Original clocks are not inferred from remaining time or timing profiles. Engine search, model, and qualified policy remain unchanged. |
| SP-003 | Verified operational documentation update | Installed readiness and targeted regressions must remain distinct from general strength evidence. | The current signed Vector 0.1.4 wrapper preserves the qualified 0.1.3 runtime/source 31d. Qualification remains bounded to its documented 180000+0 control and campaign. Keep failed trials failed and distinguish the 80 mixed-game campaign from 32 selected-policy games. |

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
cases with no skips. Independent review and packaging acceptance remain pending;
the existing 0.3.59 installation remains the repair base.

Future proposals remain open for additions. Each entry should identify the
concrete gap, proposed semantic owner, compatibility effect, qualification limit,
and user discussion outcome before becoming accepted behavior.
