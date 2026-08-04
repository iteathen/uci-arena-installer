# Installer PR #4 hostile review — native process-tree lifecycle

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-installer`  
PR: #4 `agent/harden-native-process-tree-stop`  
Reviewed head: `cd061f11afbc9997710eb2f33c090311c5e4a485`  
Base: stale `agent/release-assembly` SHA `c43cdd659bab28bc96bc81d407ab4e7e10c9ac33`  
Formal review: `4850781709`  
Disposition: **changes required; stale ancestry; keep draft**

## Confirmed blockers

1. Natural child exit leaves a joinable reader and owned process resources; a later `start()` can invoke `std::terminate` on Linux and Windows.
2. Reader-thread construction failure occurs after publishing process/pipe/handle ownership and can leave a live child tree with dirty adapter state.
3. Linux records kill failure and then enters unbounded blocking `waitpid`, so stop/destruction can hang when termination authority failed.
4. Child capability inheritance is not exact: Linux can inherit unrelated non-CLOEXEC descriptors; Windows uses `bInheritHandles=TRUE` without an exact handle list.
5. Windows startup-failure cleanup calls termination but does not verify process/tree exit before discarding handles.
6. Windows ignores reader-cancellation failure and can block indefinitely joining a synchronous `ReadFile` thread.
7. Windows performs one `WriteFile`; a partial write mutates the child protocol before failure and differs from Linux loop-to-completion semantics.
8. I/O remains byte-unbounded and control-lossy: no-newline output can grow indefinitely, queue limits count lines rather than bytes, and a full queue may evict control traffic.
9. Process-group assumptions are tested only for one simple child/descendant case and do not cover leader exit, group/session escape, startup group failure, or external reaping.
10. Full native Windows/Linux tests and real Job Object evidence are absent.
11. The PR is now `mergeable=false`; its recorded zero-behind claim is stale because authoritative PR #2 advanced.

## Required correction

- rebuild on current Installer PR #2 ancestry;
- explicit lifecycle state machine with transactional start/finalize/reuse;
- exact descriptor/handle inheritance;
- bounded termination and reader cancellation on every failure path;
- complete partial writes and bounded input/output queues;
- adversarial process-group/Job Object and startup-failure tests.

## Audit consequence

PR #4 is not acceptance evidence. Installer PR #5 and PR #11 remain blocked because they depend on this process-lifecycle ancestry.