# Local Sol -> Spark relay

Status: isolated tooling experiment on `agent/local-spark-relay`; do not merge into `main` during the current Wave 2 foundational integration sequence.

## Purpose

The relay removes manual copying between the organizing Sol conversation and local Codex/Spark.

Sol writes a machine-marked `UAI-SPARK-DISPATCH v1` comment on the active PR. A local process reads only owner-authored dispatches, exact-head guards the PR, creates a clean detached relay worktree, invokes Codex/Spark, and writes a `UAI-SPARK-RESULT v1` comment back to the same PR.

Spark does not choose the next issue, commit, push, merge, or mark a PR ready. The relay itself performs any allowed commit/push only after the Spark report, exact-head check, path-scope check, `git diff --check`, clean-tree check, ancestry check, and a second remote-head check.

The runner also prepends Windows command shims that reject `python`, `python3`, `py`, `pip`, and `pip3`; the dispatch/prompt prohibition remains authoritative as well.

## Prerequisites

- existing authenticated `gh` CLI;
- existing authenticated Codex CLI;
- Git;
- Node.js;
- a Codex profile named `spark` that selects GPT-5.3-Codex-Spark.

If the profile does not already exist, add this to the local Codex config using the model name exposed by the installed Codex version:

```toml
[profiles.spark]
model = "gpt-5.3-codex-spark"
```

If the local Codex installation exposes Spark under a different current identifier, keep the profile name `spark` and change only the profile's model value. The relay intentionally targets the profile rather than embedding model-selection policy throughout project prompts.

## Bootstrap without touching the dirty primary worktree

From PowerShell:

```powershell
Set-Location C:\chess_engine\uci-arena-installer
git fetch origin agent/local-spark-relay

git show origin/agent/local-spark-relay:tools/agent-relay.mjs |
  Set-Content -Encoding utf8 C:\chess_engine\uai-agent-relay.mjs

node C:\chess_engine\uai-agent-relay.mjs --self-test
```

The extracted runner is outside the repository and does not change, reset, stash, or clean the primary checkout.

## Run one pending dispatch

```powershell
node C:\chess_engine\uai-agent-relay.mjs `
  --repo iteathen/uci-arena-installer `
  --pr 140 `
  --checkout C:\chess_engine\uci-arena-installer `
  --profile spark `
  --once
```

`--once` is the safest initial mode: it processes at most one unconsumed dispatch and exits.

## Watch continuously

After one-shot operation is proven locally:

```powershell
node C:\chess_engine\uai-agent-relay.mjs `
  --repo iteathen/uci-arena-installer `
  --pr 140 `
  --checkout C:\chess_engine\uci-arena-installer `
  --profile spark `
  --poll-seconds 15
```

The watcher may stay open in a dedicated terminal. It acts only on uniquely identified dispatch comments whose metadata matches the configured repository/PR and whose author matches the configured GitHub owner (default: repository owner).

## Dispatch contract

A dispatch begins with:

```text
<!-- UAI-SPARK-DISPATCH v1
{"version":1,"dispatch_id":"unique-id","repo":"iteathen/uci-arena-installer","pr":140,"expected_head":"40-character-sha","target_branch":"branch/name","allowed_paths":["path/a"],"push_statuses":["completed"]}
-->
```

Everything after the marker is the Sol-owned bounded task packet.

The relay refuses a dispatch when the PR head/branch moved, an existing relay worktree is dirty or at the wrong SHA, Spark creates a commit itself, changed files escape `allowed_paths`, required report status is absent, diff checks fail, commit ancestry is wrong, or the remote PR head changes before push.

Validation-only dispatches are supported: when Spark leaves the exact head unchanged and reports its results, the relay posts the result with `push_status: no_changes` and does not create a pointless commit.

## Integration policy

This relay is process tooling, not Installer runtime behavior. During Wave 2 it remains isolated so `main` does not move merely to introduce orchestration tooling and force foundational FI/ED/ACQ/ARC branches to rebase. Review/integrate the relay separately after the current foundational integration sequence reaches a stable point.
