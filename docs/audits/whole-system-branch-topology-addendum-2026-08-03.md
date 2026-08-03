# Whole-system sanity audit — branch topology addendum

Date: 2026-08-03  
Status: active audit evidence; no release-readiness conclusion

## Finding

A correct repair that is not an ancestor of the reviewed release branch is not
part of the product. Several repositories had parallel pull-request branches
from `main` that contained valid fixes but were absent from the documented
primary audit chain. Merging only the primary pull requests would therefore
have silently reintroduced corrected defects.

This is classified as a release-assembly and review-integrity defect rather
than ordinary Git housekeeping. Review order, immutable component candidates,
and final suite composition must identify one authoritative ancestry for every
accepted fix.

## Evidence Service

Authoritative stack after correction:

```text
PR #10 -> PR #13 -> PR #14 -> PR #15 -> PR #16 -> PR #17
```

Parallel PRs #11 and #12 were absent from PR #16. Their valid fixes were merged
into current-source-aware PR #17:

- provider-neutral external foreground compute-owner roots;
- bounded canonical/legacy configuration authority;
- bounded model identifiers;
- exact non-symlink engine/model path types;
- legacy target revalidation;
- focused adversarial tests and operator documentation.

PRs #11 and #12 are closed as superseded. PR #17 is mergeable against PR #16.
No later runtime-root, CLI, streaming export/show, or transactional-prune work
was replaced by an older file snapshot.

## Engine

Authoritative stack after correction:

```text
PR #39 -> PR #44 -> PR #47
```

The parallel governance chain PR #40 -> PR #41 contained valid Engine product
identity and shared resource-lease heartbeat fixes but was not an ancestor of
PR #39. PR #47 carries those fixes onto the primary Engine chain while
preserving PR #39/#44 evaluation, packaging, and model-boundary work.

PR #42 is intentionally excluded. Its strict FEN validation intent is valid,
but its implementation renamed `Position::from_fen` through a translation-unit
macro and source-specific compile definition. That mechanism is rejected.
Issue #48 and branch `agent/canonical-fen-admission-v2` require one out-of-line
public parser with strict admission at the existing boundary and no hot-path or
preprocessor alias.

Current Engine work delegated through prepared, spec-bearing branches:

- issue #43: lock-free transposition-table publication;
- issue #45: provider-neutral inference port;
- issue #46: concrete Engine composition root, blocked on #45;
- issue #48: canonical FEN admission without macro aliasing.

The TT, inference, and composition branch names were rebased onto PR #47's
authoritative ancestry before delegation.

## Manager

Primary Manager audit PR: #54.

PR #67, SQLite storage hardening, was retargeted to PR #54 and is mergeable.
The following main-based repairs conflict with owners already modified by #54
and are closed as source evidence rather than merged by whole-file replacement:

- PR #65 -> issue #69, LocalAuth reconciliation on
  `agent/harden-local-auth-after-release-assembly`;
- PR #66 -> issue #70, UCI admission reconciliation on
  `agent/harden-uci-probe-after-release-assembly`;
- PR #68 -> issue #71, bounded match-plan replay on
  `agent/bound-match-plan-after-release-assembly`.

Each prepared branch starts from PR #54. The tasks require semantic three-way
reconciliation and preservation of PR #54's newer control-plane, discovery,
authentication, and specification-registry behavior.

## Installer

Primary Installer audit PR: #2.

The following focused PRs were retargeted onto the authoritative Installer
branch and are mergeable:

- PR #3: global indexed-asset result bound;
- PR #4: native process-tree shutdown;
- PR #5: typed service-control actions, stacked on PR #4.

No implementation replay was required because GitHub's three-way merge proves
that the focused ownership changes coexist with PR #2.

## Book Forge

Authoritative stack:

```text
PR #8 -> PR #11
```

PR #9's source behavior was already present in PR #8: exceptions from the
periodic acquisition resource-permission check abort the in-flight request and
produce the typed backoff path. Only its focused regression test was absent.
Commit `f698d49` adds that test directly to PR #11, and PR #9 is closed as a
duplicate.

PR #10, unconditional daemon-lock release, is retargeted to PR #11 and is
mergeable.

## Lichess Bot

PR #3 remains the sole coherent provider audit branch. No orphan repair chain
was found.

## Merge and validation rules

1. Do not merge a child before its declared base or retarget it to `main`
   without rechecking the complete diff.
2. Do not accept a whole-file copy from a parallel branch when the base changed
   the same owner; perform a semantic three-way reconciliation.
3. Every final component candidate must be built from a commit that contains
   every accepted repository-local repair in its ancestry.
4. Installer release locks must identify those exact source commits and newly
   built immutable candidates; old candidates cannot claim branch-only fixes.
5. Re-run repository-native tests after ancestry convergence. Prior branch
   tests do not prove the integrated tree.
6. The final installed Windows/Linux cycle remains required under
   `PKG-E2E-011`; topology correctness does not substitute for package and
   runtime execution.

## Current validation limitation

The GitHub connector verified branch bases, changed-file sets, mergeability,
exact blob identity where used, and static diffs. This environment cannot clone
or execute the private repositories, does not supply the required Node 24.15.0
runtime, and cannot run the native Windows/Linux toolchains. No new native,
Node, package, or installed-suite result is claimed by this addendum.
