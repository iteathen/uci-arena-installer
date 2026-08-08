# Jules-derived codebase contamination map — 2026-08-04

Status: **active forensic containment; no soundness or release-readiness conclusion**

Purpose: locate code and governing artifacts that reached authoritative default branches through direct Jules output or an explicitly Jules-derived reconciliation. This audit distinguishes code Jules introduced from defects that already existed and Jules merely failed to repair.

## Clean-reference opportunity

The operator reports that the local desktop work directory may predate Jules. Treat that directory as potentially irreplaceable forensic evidence.

Do not pull, fetch-and-reset, merge, rebase, checkout another branch, run an agent, run `workspace_cleanup.mjs`, or modify generated/source files in that directory until its full filesystem copy, Git HEAD, branch, remotes, status, submodules, untracked files, and timestamps are captured.

Known contamination cutoffs:

- Engine clean predecessor: `912f9d8067f70c5f711873d83449be186a385c9f`; Jules-derived cleanup merge: `68e181fe77c159b997edeb40e8ecf6e237c0ded6`.
- Manager clean predecessor: `cf606950457d2fe009489875043c952290b3c142`; direct Jules merge: `9460c3ccec74413c52dbd99540f17e13dba73232`.

A local HEAD at or before the clean predecessor is a strong clean-reference candidate. A later HEAD can still be useful if the affected files differ from contaminated `main`.

## Finding A — confirmed introduced Engine damage

Origin: merged Engine PR #16, source head `9475e96b8477426595036970a2a500b984b0b7a6`, merge `68e181fe77c159b997edeb40e8ecf6e237c0ded6`. The PR explicitly states it reconciled and corrected an original Jules branch. It merged with no submitted review and no PR discussion.

### Files introduced or modified

1. `.gitignore`
2. `docs/local_workspace_cleanup.md`
3. `specs/improvement_plans/local_workspace_cleanup_controls_plan.yaml`
4. `specs/index.yaml`
5. `tools/spec_preflight_check.py`
6. `tools/workspace_cleanup.mjs`
7. `tools/workspace_cleanup.test.mjs`
8. `tools/workspace_cleanup_lib.mjs`

The cleanup library and command remain present on Engine `main`. The governing cleanup spec remains `active`, the documentation still advertises the destructive `apply` command, the preflight router treats the spec as authority, and tests certify the incomplete implementation.

### Confirmed damage

- Adds destructive recursive rename/delete authority that did not exist at the clean predecessor.
- Final snapshot and later rename operate through mutable pathnames, so a replacement object can be quarantined and deleted.
- Recursive traversal, hashing, and deletion use pathname observations rather than retained exact-object authority.
- Windows handle/file-ID/reparse-safe authority is absent.
- Entry count, depth, file count, individual and aggregate bytes, memory, time, Git output, diagnostics, and cancellation are unbounded.
- Provider reproducibility is inferred from a workflow substring; CMake reproducibility is inferred from mutable cache text.
- Manifest/result publication can overwrite or follow unsafe paths and lacks a committed durability/result model.
- Sequential deletion permits partial irreversible success and ambiguous final reporting.
- Tests do not trigger actual object-replacement races, Windows reparse behavior, resource exhaustion, partial deletion, rollback collisions, or result-publication failure.
- Active specification and operator documentation materially overclaim safety and can train future agents to preserve the damaged design.

### Blast radius

- **Source/developer worktrees:** high. Manual `apply` can delete valuable untracked or generated directories.
- **Agent governance:** high. Active spec, preflight routing, tests, and docs can cause future agents to reuse or defend the unsafe subsystem.
- **Installed UCI Arena Engine component:** currently limited. The Engine CMake install and provider workflow stage the installed engine/referee payload, not the repository cleanup scripts. No evidence presently shows `workspace_cleanup.mjs` inside the shipped engine payload.
- **Prior filesystem execution:** unresolved. The earlier cleanup PR description claimed deletion of eleven directories totaling about 632.2 MiB. That self-report is not trusted evidence that every deleted byte was reproducible. A pre-Jules local directory may help determine whether anything unique disappeared.

Immediate rule: do not run `node tools/workspace_cleanup.mjs apply` on valuable worktrees. Engine issue #64 owns removal/replacement.

## Finding B — confirmed Manager contamination; net regression attribution narrower

Origin: directly merged Jules PR #4, merge `9460c3ccec74413c52dbd99540f17e13dba73232`.

Exact direct delta: seventeen additions in `src/application/engine_onboarding.cpp`, splitting `is_directory`, `is_symlink`, and `is_regular_file` checks and clearing errors after each.

The exact patch remains present on Manager `main` inside `EngineOnboarding::append_explicit_root`.

### Attribution

The broader design defects—unbounded total traversal, mutable-path candidate identity, separate discovery and later probing, and lack of Windows reparse authority—already existed in the pre-Jules file at `cf606950457d2fe009489875043c952290b3c142`. They must not be falsely attributed to the seventeen Jules lines.

The Jules patch is still contaminated and inadequately reviewed. It changes error behavior by clearing classification failures and continuing, but does not disable recursion for an unclassified directory and does not establish a bounded or exact-object boundary. Whether every changed line is a behavioral regression requires adversarial native testing; the complete owner boundary is economically cheaper and safer to rewrite than to preserve.

### Blast radius

- `engine_onboarding.cpp` is compiled into `arena_core`.
- `arena_core` is linked into `arena-service`, CLI, setup, broker, and other Manager binaries.
- Manager CMake installs those binaries on Windows and Linux.
- The authenticated `/api/v1/engine-candidates?root=...` service endpoint calls `EngineOnboarding::discover`, so explicit-root traversal is live installed-product behavior.
- Current tests cover a small happy-path nested executable but not permission errors, large irrelevant trees, disappearing entries, object replacement, junctions/reparse points, deadline/cancellation, or complete work metrics.

Consequences include unbounded service work, fail-open recursion after classification uncertainty, and candidate pathname substitution between discovery and later probe/registration. Manager issue #81 owns full reconstruction.

## Finding C — quarantined, not default-branch damage

The following rejected or reconstructed Jules-derived work is not merged into current default branches and must remain quarantined:

- Engine PR #52, replayed from Jules PR #50.
- Engine PR #63 and every reviewed/empty head on its Jules branch.
- Manager PR #73, reconstructed after empty Jules PR #72.
- Manager PRs #78, #79, and #80, reconstructed after rejected Jules PR #74.
- Closed review overlays Engine PR #53 and Manager PR #75.
- Closed Jules PRs #32, #49, #50, #51, #55, #60, #61, #72, #74, and #77.

These branches are contamination sources if a local worktree, cherry-pick, merge, or copy uses them, but they are not presently default-branch source damage.

## Finding D — no known Jules contamination on remaining default branches

Current PR and default-branch history found no direct or explicitly reconciled Jules code merged into:

- `iteathen/uci-arena-book-forge` main;
- `iteathen/uci-arena-evidence-service` main;
- `iteathen/uci-arena-lichess-bot` main;
- `iteathen/uci-arena-installer` main.

Those repositories have independent audit defects and open unsafe branches, but they are not currently classified as Jules-code contamination. Installer PR #2 contains audit records, not accepted Jules production code.

## Forensic comparison procedure for the desktop directory

First make a full byte-preserving copy of the directory, including `.git`, untracked files, ignored files, build trees, runtime state, and timestamps. Work only from the copy afterward.

Capture for each repository without network mutation:

```powershell
$PWD.Path
git rev-parse --show-toplevel
git rev-parse HEAD
git branch --show-current
git remote -v
git status --porcelain=v2 --branch --untracked-files=all
git submodule status --recursive
git log -1 --format=fuller
```

Then test known contamination ancestry where the commit objects are available:

```powershell
git merge-base --is-ancestor 68e181fe77c159b997edeb40e8ecf6e237c0ded6 HEAD
$LASTEXITCODE  # Engine: 0 means cleanup contamination is an ancestor; 1 means it is not.

git merge-base --is-ancestor 9460c3ccec74413c52dbd99540f17e13dba73232 HEAD
$LASTEXITCODE  # Manager: 0 means direct Jules merge is an ancestor; 1 means it is not.
```

Exit code 128 means the commit object is absent locally and must not be interpreted alone as proof of cleanliness.

After capture, compare the desktop copy against both the clean predecessor and current `main` at exact paths. Preserve uncommitted and ignored files as possible recovery evidence; do not use a Git-only bundle as the sole backup because it omits those files.

## Current containment conclusion

Confirmed live default-branch contamination is concentrated in two places:

1. Engine workspace cleanup subsystem and its supporting governance/tests/docs—introduced damage, source-worktree destructive risk, not currently in the installed engine payload.
2. Manager explicit-root engine discovery—direct Jules patch inside a pre-existing unsafe owner boundary, shipped in installed Manager binaries.

No claim is made that this map is complete until the pre-Jules desktop tree is captured and compared. No soundness or release-readiness conclusion exists. `PKG-E2E-011` remains unknown.
