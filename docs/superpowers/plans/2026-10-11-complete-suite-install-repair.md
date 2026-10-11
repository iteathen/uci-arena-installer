# Complete Suite Installation Repair Plan

> **For agentic workers:** Use the existing owner worktrees and execute the bounded component tasks, with independent review before merging and installation.

**Goal:** Install a working complete UCI Arena suite containing the corrected cuda-js engine and services, preserving user data and keeping the bot stopped.

**Architecture:** Each provider repairs and signs its own component. Installer consumes exact signed archives through lock v3, uses the existing atomic directory payload and public lifecycle transaction, and verifies the installed public surfaces.

**Tech Stack:** Existing Node26 release tools, native Windows installer, public provider SDKs, cuda-js/Tensor/MCGS engine.

**Spec:** `specs/00_READ_FIRST.yaml`, `specs/installation_contract_v1.yaml`, `specs/suite_lego_boundaries_v1.yaml`, `specs/provider_lifecycle_transactional_v1.yaml`, `specs/release_provenance_v1.yaml`; provider accepted specs own component repairs.

## Global Constraints

- Never read a component repository's source tree during composition.
- Never place credentials in manifests, logs, arguments, or receipts.
- Never weaken provenance or repository visibility to make composition pass.
- Preserve provider workspaces, archives, knowledge and the last working installation.
- Preserve signed Vector0.1.3/source31d and its actual qualified180+0 policy; no C++ chess or search workaround.
- Bot stays stopped with startup disabled; no disk cleanup or process-name/PID kill.

## Review Focus

- Inconsistent retained Task XML and typed action must reject before any provider effect.
- Unknown game initial clock must not be inferred from residual clock or permit unqualified search allocation.
- Disabled bot intent must survive installation and subsequent startup reconciliation.
- Uninstall and fresh installation on E must preserve book authority and game history through public owner operations.
- Failure/rollback/cleanup must remain distinct and must not hide ambiguous provider side effects.

### Task1: Repair provider acceptance boundaries

- [x] Book0.4.9 signed and independently reviewed; isolated real owned Windows uninstall/copy/retained-C-backup/fresh-E-install sequence passed.
- [x] Bot1.0.45 initial-clock handoff, portable timing-policy binding and offline restore independently reviewed; official signed payload and inventory verified.
- [x] Fresh signed43 interlock reports activeWork0, disabled/inactive; old maintenance recovery is unnecessary.
- [x] Vector0.1.3 signed bytes, actual policy, native checks and stopped configuration already qualified; reuse unchanged.

### Task2: Compose the corrected suite

**Files:** `compositions/complete-arena-0.3.55.json`, `specs/installation_contract_v1.yaml`; external lock/candidate directories are generated from exact signed inputs.

- [ ] Finalize Complete0.3.55 with Vector0.1.3, Book0.4.9, Bot1.0.45, Manager0.3.28 and Evidence0.3.16 after the remaining provider reviews/signatures.
- [x] Composition source checks and independent review passed and exact reviewed head merged.
- [ ] Assemble unified lock v3 from signed candidate rows and rerun official public provenance admission for all eight components.

### Task3: Build and exercise the installer

- [ ] Run existing `clean-build.mjs --complete-development --platform windows-x86_64 --validation-mode official` with exact lock, policy and candidate map; reuse current toolchain, no hand-maintained per-file installer components.
- [ ] Independently review exact resulting artifacts and full component closure.
- [x] Installer current-generation MSI-owned selected-root binding merged after native and isolated actual MSI install/repair/conflict/rollback/uninstall checks. Manager integration remains pending final review/signature; pending IL05/06/07 is not claimed.
- [ ] Preserve user data, run normal personal-scope uninstall, retain C originals, install freshly on E and restore artifacts through public owning operations; retain failure, compensation and cleanup evidence.
- [ ] Fix any proven owning-component defect rather than bypass it, and repeat only affected checks.

### Task4: Verify installed concerted operation

- [ ] Verify installed receipt versions and public Manager/provider health/configuration surfaces, swappable UI engine selection and book/Evidence interoperability.
- [ ] Replay reported engine fault through installed bot options, preserving actual180+0 policy and between-game binding.
- [ ] Verify bot disabled and zero instances; record exact installed acceptance and remaining evidence limits.

Existing historical failed installer artifacts and receipts remain immutable evidence. This plan changes no provider contract by itself.
