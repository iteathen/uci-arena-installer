# Installer PR #11 hostile review — platform-neutral service-control result

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-installer`  
PR: #11 `agent/platform-neutral-service-control-result`  
Reviewed head: `c40edfcbe12a5e5187a4b1543aee610f40d47f02`  
Base: PR #5 / `agent/type-service-control-actions` at `16fe257ce6d55714a4d87efcea4ffe1314f4a2b7`  
Formal review: `4850690366`  
Disposition: **changes required; keep draft**

## Purpose and ownership

PR #11 replaces incompatible raw integer service-control results with one Installer-owned typed result. That direction is necessary, but the current implementation violates the intended adapter boundary and does not provide the promised bounded, substitutable Windows/Linux semantics.

## Confirmed blockers

1. **Raw platform vocabulary leaks into the neutral public header.** `service_control.hpp` maps systemd `LoadState`/`ActiveState` strings and raw Win32 `SERVICE_*`/error numeric values. Issue #6 explicitly assigns SCM/systemd mapping to platform adapters. The spec was changed to codify the opposite ownership rather than satisfy the issue and LEGO boundary.
2. **Windows resets the transition budget across helper phases.** Start and restart can consume multiple independent 30-second deadlines despite the stated one-deadline contract.
3. **Windows can silently retry a failed pending start.** A prior `SERVICE_START_PENDING` transition that resolves stopped can lead to a second `StartServiceW()` call rather than reporting the observed failure. A newly started service that returns stopped with zero exit code waits until timeout although running is no longer reachable without another start.
4. **Windows mutation failures discard known state.** `StartServiceW` and `ControlService` failures map state only from the error code, losing the status observed immediately before mutation. Linux queries after a failed mutation and preserves state, so caller semantics differ.
5. **SCM infrastructure unavailability is misreported as target-service unavailability.** `RPC_S_SERVER_UNAVAILABLE` maps to `ServiceState::unavailable`, inventing target state when only the control infrastructure failed.
6. **Linux can lose its timeout before polling.** `F_GETFL`/`F_SETFL` failures are ignored. If the output pipe remains blocking, the first drain can block indefinitely before the 30-second deadline is evaluated.
7. **Timeout state authority differs by platform.** Linux mutation timeout returns unknown without a final observation; Windows returns the last observed state.
8. **Linux descriptor/termination closure is unproved.** `pipe()` lacks CLOEXEC, unrelated non-CLOEXEC descriptors may cross into `systemctl`, and timeout cleanup uses a direct-child kill plus unbounded blocking `waitpid`.
9. **Outcome semantics for unavailable status are not demonstrated at caller level.** A successfully identified missing service is encoded as a failed status action, but the meaning of outcome versus target condition is not proven through substitution tests.
10. **Only pure header tests were added.** No production-adapter tests cover systemd parsing/process failures or Windows deadline, pending-state, mutation-failure, timeout, RPC, and service-specific behavior. Real booted-systemd and SCM validation remains absent.

## Required architectural correction

- neutral port owns only project types, names, bounds, and platform-independent result semantics;
- platform-private mapping owners translate systemd/SCM observations;
- one absolute deadline flows through every phase of an action;
- last trustworthy service state is preserved on platform failure;
- target absence and control-infrastructure failure are distinct;
- Linux nonblocking setup and child cleanup are exact and bounded;
- equivalent Windows/Linux scenarios pass caller-level substitution tests.

## Audit consequence

PR #11 is not acceptance or release evidence and issue #6 remains open. The Installer stack cannot claim platform-neutral service control until the corrected branch passes production-adapter tests and real Windows/systemd integration.