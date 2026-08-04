# Jules task — bind verified UCI executable identity to the launched process

Do not launch while another broad Jules session is active. Use the official Jules REST API against the exact branch below.

## Exact starting state

- repository: `iteathen/uci-arena-manager`
- starting branch: `agent/bind-verified-uci-launch-identity`
- expected starting SHA: `683d7ae20a161d0b40d176d82173a87a961c77ea`
- parent: PR #80 branch `agent/bound-uci-probe-wire-framing`
- source issue: #70

Verify branch and SHA before editing. Stop with exact evidence if they differ.

## Objective

Complete the remaining Manager #70 process/path boundary so the SHA-256, executable identity, working-directory identity, and actual launched process all refer to the same admitted objects. Remove the current `UciProcess::fingerprint(path)` then `NativeProcess::start(path)` check/use race.

Do not modify UCI option grammar or raw framing already owned by PRs #78–#80.

## Read first

- `AGENTS.md`
- repository read-first and spec registry
- issue #70 and failed PR #74 review
- PRs #78, #79, and #80
- `include/arena/ports/native_process.hpp`
- both platform `process_platform.cpp` files
- `src/adapters/uci_process.cpp`
- onboarding/receipt call sites and process tests

## Public contract

Add one Manager-owned verified launch result/identity, for example:

- SHA-256 of exact launched executable bytes;
- exact platform file identity in an opaque bounded diagnostic form;
- exact size;
- canonical admitted executable path and working-directory path for operator evidence only;
- process id;
- platform-neutral launch outcome/error.

`NativeProcess::start()` must return/store this identity. `UciProbeResult.fingerprint` must come from the NativeProcess launch identity, never a separate path read.

Callers must not interpret raw inode, volume serial, Win32 code, or platform names as domain meaning.

## Shared admission

Before mutation/launch:

- executable and working directory are absolute, nonempty, bounded, and contain no NUL/control ambiguity;
- every existing ancestor and final object are admitted without following a substituted symlink/junction/reparse point;
- executable is one regular file; working directory is one directory;
- arguments are bounded by count, per-argument bytes, and cumulative bytes; embedded NUL is rejected;
- timeout is a positive safe bounded duration;
- override cardinality/name/value byte shape is rejected before launch where advertisement-independent;
- no shell strings or command-line reinterpretation;
- all handles/descriptors and child processes are cleaned on every failure path.

## Linux exact launch

A path recheck is not sufficient.

- Open ancestors/final executable with no-follow semantics and exact descriptor identity.
- Open the working directory as a no-follow directory descriptor and use `fchdir()` in the child.
- Acquire an enforceable read lease or equivalent mechanism proving no writer can mutate the executable between digest and exec. If the filesystem/kernel cannot provide this, fail closed with a typed unsupported-authority result; do not silently downgrade.
- Hash through the admitted executable descriptor, compare pre/post descriptor identity/size/timestamps, and launch the same descriptor using `fexecve()` or exact `execveat(..., AT_EMPTY_PATH)` semantics.
- Keep the mutation-prevention authority for at least the launch handshake, preferably the process lifetime if required for exact model provenance.
- Use an explicit parent/child error pipe so pre-exec `fchdir`, argument, or `fexecve` failure is reported and never mistaken for a running engine.

## Windows exact launch

- Open executable with `CreateFileW` using `FILE_FLAG_OPEN_REPARSE_POINT`, reject reparse/unsupported objects, and capture `FILE_ID_INFO`, size, and basic timestamps.
- Deny write/delete sharing while allowing only the access required for `CreateProcessW`; retain the verified executable handle through launch and identity verification so replacement or mutation through any hard-link name cannot occur.
- Hash through that exact handle and recheck identity/size/time after hashing.
- Open and retain the working-directory handle with reparse-safe directory semantics and deny replacement.
- Launch using the path resolved from the verified handle, while the deny-write/delete handle remains open.
- After `CreateProcessW`, query the process image and verify it resolves to the same file identity before resuming useful work. Failure terminates the Job Object and returns a typed failure.
- Keep the verified handle(s) for the required launch/process lifetime and close them on every path.

Do not claim that a `stat`/hash/path `CreateProcessW` sequence binds identity.

## Receipt/onboarding integration

- The installed receipt/manifest remains preferred metadata but never substitutes for the exact runtime launch identity.
- If a receipt declares a digest, require exact equality with the launch identity.
- Store/report only the neutral launch identity and exact digest needed for provenance; raw paths/codes remain bounded operator evidence.
- Runtime model/provider identity remains separate and preserved.

## Tests

Use production code and cover:

- normal executable and working directory on Linux and Windows;
- executable final symlink, ancestor symlink, Windows junction/reparse point, directory/special object;
- working-directory final/ancestor link or replacement;
- replacement before open, during hash, after hash, during launch, and immediately after process creation;
- same-size mutation and hard-link mutation attempt;
- unsupported Linux lease/filesystem fails closed;
- pre-exec `fchdir`/`fexecve` failure and exact child error handshake;
- Windows CreateProcess, image-identity mismatch, Job Object cleanup, share-denied mutation/replacement;
- argument count/per-value/cumulative bounds, NUL, invalid UTF-8 where applicable, and timeout bounds before launch;
- receipt digest match/mismatch;
- proof that `UciProbeResult.fingerprint` is the launched-object digest;
- repeated launch/stop without leaked handles, descriptors, jobs, leases, pipes, or children;
- package-installed Windows and Linux behavior with no skipped parity claims.

Add race tests that coordinate the attacker and launch owner deterministically. Do not use timing-only sleeps as proof.

## Performance

- Hash exactly once per admitted launch identity, streaming through a fixed bounded buffer.
- Do not reread/hash the executable in `UciProcess` or onboarding after the platform owner returns identity.
- No per-output-line impact; framing remains owned by PR #80.
- Record representative executable-size hash/launch overhead separately from steady-state engine performance.

## Validation

Run required preflight/spec checks, complete Linux and Windows builds/CTest, deterministic race tests, process/handle leak tests, and package-installed probes. Record exact commands/output. Automatic CI remains disabled.

## Delivery

Open a draft PR from `agent/bind-verified-uci-launch-identity` to `agent/bound-uci-probe-wire-framing`. Keep changes inside the NativeProcess launch identity owner, UCI probe/onboarding integration, governing specs, and focused tests. Do not alter storage, LocalAuth, Installer, option grammar, or raw framing semantics. Do not merge.
