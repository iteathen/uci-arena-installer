# Manager PR #80 hostile review — bounded UCI wire framing

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-manager`  
PR: #80 `agent/bound-uci-probe-wire-framing`  
Reviewed head: `683d7ae20a161d0b40d176d82173a87a961c77ea`  
Base: PR #79 / `agent/integrate-uci-option-contract` at `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`  
Formal review: `4850674462`  
Disposition: **changes required; keep draft**

## Purpose and ownership

PR #80 attempts to establish the Manager-owned raw-byte process framing boundary for UCI probes while preserving bounded control traffic and permitting explicitly observational `info ` output to be dropped under pressure. The direction is valid and the branch ancestry is correct, but the implementation is not yet a sound project-owned process/UCI boundary.

## Confirmed blockers

1. **Linux writes can terminate Manager through SIGPIPE.** `write_line()` checks process state and then uses `write()` on a pipe without suppressing or safely consuming SIGPIPE. A child can close stdin between those operations. A strict local C++20 probe reproduced the platform behavior as exit `141`, signal `13`, so the intended exception path is not reliable.
2. **Natural-exit restart is unsafe on both platforms.** `start()` rejects only a currently running child. A naturally exited child leaves a joinable reader and owned descriptors/handles. A second `start()` overwrites resources and assigns into the joinable thread, invoking `std::terminate`. A strict local probe reproduced exit `134` with `terminate called without an active exception`.
3. **Protocol validity depends on queue pressure.** Raw `info ` prefix classification happens before UCI wire validation. Malformed/control-bearing `info` output is fatal when dequeued but may be silently dropped or evicted under pressure. Identical engine output can therefore pass or fail based on scheduling.
4. **LEGO authority is duplicated.** Complete UTF-8/control scanners exist in both `UciOptionContract::validate_wire_line()` and `uci_option_contract.cpp::require_text()`. Limit/input validation is separately duplicated in the Linux and Windows adapters.
5. **UCI policy leaks into the generic process port.** `NativeProcessIoLimits` defaults its droppable prefix to `"info "`, so a platform-neutral process brick embeds UCI behavior rather than receiving an explicit adapter-owned policy.
6. **Windows stop is not proven bounded.** `stop()` handles only `WAIT_TIMEOUT`, ignores wait/termination failures, and joins a reader that may remain blocked while stdout stays open. `reset_failed_start()` likewise discards termination/wait outcomes.
7. **Timeout bounds are incomplete.** Negative values are rejected, but large durations can wrap Windows `DWORD` conversion or create overflow/effectively unbounded deadlines.
8. **Tests do not falsify the dangerous paths.** Missing evidence includes SIGPIPE survival, natural-exit reuse, malformed `info` under pressure, exact queue boundaries, CRLF boundaries, forced partial writes, reader failure after startup, Windows wait/termination/read cancellation, and native Windows parity. The `framing-info-burst` test remains scheduler-sensitive.
9. **No repository-native or Windows validation exists.** Automatic CI is correctly disabled, but no checks exist for this head and the PR itself records complete Linux/Windows suites as still required.

## Required architectural correction

- one provider-neutral process-I/O policy validator outside platform adapters;
- one dedicated UCI wire-text contract reused by metadata validation instead of duplicated scanners;
- lossless/default-neutral `NativeProcess` behavior, with UCI observation classification supplied explicitly by the UCI adapter;
- an exact lifecycle state machine covering running, natural exit, failure, cleanup, stop, and restart;
- POSIX closed-pipe writes converted to bounded failures without process termination;
- bounded Windows termination and reader cancellation with typed failure evidence;
- deterministic adversarial tests on both supported platforms.

## Audit consequence

PR #80 is not acceptance or release evidence. Manager issue #70 remains open not only for exact hash-to-launched-object identity, but also for correction of this framing/lifecycle slice. Any downstream verified-launch task must start from a corrected PR #80 ancestry, not the currently reviewed head.
