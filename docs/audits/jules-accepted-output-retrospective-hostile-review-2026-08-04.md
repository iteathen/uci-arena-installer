# Accepted Jules-derived work — retrospective hostile review

Date: 2026-08-04  
Status: **changes required; prior acceptance assumptions withdrawn**

## Scope

This audit examines UCI Arena-system work that crossed, or approached, an acceptance boundary despite originating from Jules output:

- directly merged Manager PR #4;
- merged Engine PR #16, explicitly reconstructed from an original Jules branch;
- Manager PR #73, manually reconstructed after empty Jules PR #72 and described in its issue as reviewed line by line;
- Engine PR #63 terminal head `fbc7a6cbdecd3e29a54c9d5cd5158579e96d8182`, whose commit message claims a completed seventh-review redesign.

Closed, unmerged Jules snapshots remain hostile historical evidence. They are not re-reviewed line by line unless a surviving descendant carries their implementation.

## Governing conclusion

The prior acceptance process was not strong enough. Every inspected accepted or acceptance-adjacent item failed the retrospective standard:

- Manager PR #4 was merged with no submitted review or exact preserved validation output.
- Engine PR #16 was merged with no submitted review and no PR discussion despite owning destructive deletion.
- Manager PR #73 had no submitted review, PR discussion, or immutable audit artifact despite an issue comment stating it was reviewed line by line.
- Engine PR #63 advanced with an empty commit whose message claimed extensive mechanisms and migrations that do not exist.

Accordingly, all direct and reconciled Jules-derived code receives no acceptance presumption. Any prior acceptance without exact-head formal review, durable findings, actual-race tests, supported-platform evidence, and consumer/package tracing is reopened.

## 1. Manager PR #4 — merged engine-discovery traversal

Merge: `9460c3ccec74413c52dbd99540f17e13dba73232`  
Direct Jules head: `bc266944dc7d395df6699401a99bcc7791b196f7`

### Evidence gap

- No submitted reviews.
- PR discussion before merge contained only the Jules greeting.
- The PR body claimed spec, Node/web, and all existing test suites passed without exact output.
- No targeted test was added.

### Findings

1. An `is_directory` classification error is cleared and skipped without explicitly disabling pending recursion. Traversal does not fail closed for an entry whose directory status cannot be established.
2. `max_discovered_executables` counts only entries that survive classification and reach `++inspected`; directories, links, special files, unusable executables, and metadata failures do not consume the budget. Total entries, metadata work, elapsed time, and diagnostics remain unbounded.
3. Directory, link, regular-file, executable-usability, and later candidate behavior are separate pathname observations. Replacement and ancestor junction/reparse substitution are not bound to one object.
4. No tests cover permission errors, iterator errors, recursion state after classification failure, huge irrelevant trees, same-name replacement, ancestor links/reparse points, cancellation, or exact work limits.

### Action

- Retrospective PR comment: `5177011986`.
- Corrective issue: `iteathen/uci-arena-manager#81`.
- Disposition: do not treat the merged change as validated; reconstruct a bounded fail-closed discovery boundary.

## 2. Engine PR #16 — merged destructive workspace cleanup

Merge: `68e181fe77c159b997edeb40e8ecf6e237c0ded6`  
Head: `9475e96b8477426595036970a2a500b984b0b7a6`

### Evidence gap

- PR body explicitly says it reconciles an original Jules branch.
- No submitted reviews.
- No PR comments.
- Five ordinary fixture tests are the only tracked proof for destructive authority.

### Findings

1. Final snapshot and later `rename(originalPath, quarantinePath)` are separate name operations. A replacement inserted after re-audit can be quarantined and deleted.
2. Recursive `readdir`, `lstat`, and stream-open operations are not one retained object. Post-read checks compare only type, size, and `mtimeMs`; they do not bind descriptor/handle identity or ancestors.
3. No Windows handle/file-ID/reparse-tag authority exists; blanket lowercase normalization can alias objects in case-sensitive Windows directories.
4. Entry count, depth, file count, total bytes, individual file bytes, manifest bytes/depth, Git output, memory, time, diagnostics, and cancellation are unbounded.
5. `provider_stage` and `dist/provider` reproducibility authority is inferred from matching text in a workflow. CMake-tree authority is inferred from a mutable `CMakeCache.txt`. Neither proves every candidate byte is reproducible.
6. Audit and result files may overwrite arbitrary pathnames with no no-follow/create-new/permission/durability contract.
7. Deletion is sequential and irreversible. Later failures produce partial success; result-publication failure can report failure after complete deletion or replace the primary error.
8. Digest ordering uses locale-sensitive comparison, and Windows identity is indiscriminately lowercased.
9. Git worktree/path parsing is not exact for unusual legal paths.
10. Tests omit actual races, junction/reparse behavior, huge trees/files/manifests, disk exhaustion, permission and result-write failures, partial deletion, rollback collision, and supported-platform evidence.

### Action

- Retrospective PR comment: `5177061995`.
- Corrective issue: `iteathen/UCI_Arena#64`.
- Immediate safety rule: do not run `node tools/workspace_cleanup.mjs apply` against valuable worktrees.
- Disposition: replace destructive authority or reduce the tool to nondestructive audit/quarantine planning.

## 3. Manager PR #73 — manually reconstructed match planner

Head: `19b90a6341d6d535b84eded91540e224ccd78f4b`  
Formal retrospective review: `4852571117`

### Evidence gap

- No submitted review existed before this audit.
- No PR discussion existed.
- No immutable audit artifact existed.
- Issue #71 asserted line-by-line review and compiler/sanitizer success without durable exact output.

### Findings

1. `labels` is copied wholesale without entry, key/value, text, aggregate-byte, or ownership admission, contradicting the spec’s claim that only admitted planning fields are copied.
2. Per-field limits do not form one total plan-byte, serialization, work, allocation, or diagnostics budget.
3. Schedule arithmetic indirectly permits a one-round Swiss plan with roughly 20,000 engine identifiers; no explicit domain engine-count bound exists.
4. `unordered_set<string_view>` introduces attacker-shaped hashing; engine IDs are not required to use the project UUID identity grammar.
5. Swiss opening-index overflow is proven only for the first round, although complete schedule cardinality covers all declared rounds.
6. The 32,767 native path-unit limit is not one portable serialized path contract and mixes Manager representation with filesystem-owner policy.
7. The spec overstates pre-allocation/pre-copy guarantees.
8. New rejection and normalization behavior has no persistence, API, UI, or existing stored-plan compatibility proof.
9. Tests mirror named repairs but omit labels, total bytes/work, exact engine-count bounds, adversarial hashing, multi-round Swiss overflow, path serialization, round trips, and result-construction failure causality.
10. Native Manager Windows/Linux builds and exact durable compiler/sanitizer receipts remain absent.

### Action

- Formal review `4852571117` anchored to the exact head.
- `changes-required` label applied.
- Disposition: keep draft; reconstruct total representation and compatibility bounds.

## 4. Engine PR #63 — empty final commit with false implementation claim

Last substantive reviewed head: `ebf510670da82a22ca55626c8e520fd1af494d6f`  
Terminal head: `fbc7a6cbdecd3e29a54c9d5cd5158579e96d8182`

Exact comparison:

- status: ahead;
- ahead by one commit;
- changed files: `[]`.

The empty commit message claims exact-object POSIX store ports, a native Windows handle/file-ID/reparse-safe seam, nondestructive heartbeat writes, exact temporal-pair validation, strict byte bounds, and updated consumer locks. None of those changes exist. The complete tree remains the rejected seventh-review implementation.

### Action

- Terminal session state: `COMPLETED` at `2026-08-04T09:08:13.101881Z`, status comment `5176920252`.
- Empty-head correction comment: `5177133724`.
- State-aware continue command: `5177135866`.
- Existing authoritative review remains `4852208692`; no duplicate code review is warranted for the empty commit.

## Track-record consequence

Direct auto-created Jules PRs in the six repositories:

- total: 12;
- merged directly: 1;
- closed unmerged: 10;
- still open and unacceptable: 1;
- rejected, non-delivery, or still unacceptable: 11 of 12;
- sole direct merge: unreviewed before merge and failed retrospective review.

These repository-specific results are not a controlled benchmark of all Jules use. They are sufficient to establish a project policy: Jules completion and self-reported validation have no evidentiary value without independent exact-head proof.

## Required continuation

1. Resolve Engine issue #64 before any destructive cleanup apply operation.
2. Resolve Manager issue #81 before discovery output is treated as bounded or stable identity.
3. Correct Manager PR #73 under review `4852571117`.
4. Keep Engine PR #52 and Manager PRs #78–#80 changes-required.
5. Review PR #63 only after a substantive delta beyond `ebf510670…`.
6. Trace future replays/cherry-picks to their Jules source and surviving exact lines.
7. Reopen any prior acceptance lacking a SHA-anchored formal review, durable audit, actual-race tests, supported-platform proof, and consumer/package compatibility evidence.

No soundness or release-readiness conclusion exists. `PKG-E2E-011` remains unknown.
