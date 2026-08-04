# Jules task — exact acquisition credential at use time

Do not launch while another broad Jules session is active. Use the official Jules REST API against the exact branch below.

## Exact starting state

- repository: `iteathen/uci-arena-book-forge`
- starting branch: `agent/exact-acquisition-credential-use`
- expected starting SHA: `f698d49b6f9b7bdf8105dde3c598b053502e08f0`
- authoritative parent: PR #11 branch `agent/config-contract-after-system-sanity`
- source issue: #12

Verify branch and SHA before editing. Stop with exact evidence if they differ.

## Objective

Replace `ExplorerAcquisition.credential()` with one Book Forge-owned, callback-scoped credential-use boundary that admits exact stable bytes from the exact external file at request time and keeps the verified object identity authoritative through the HTTP operation.

Configuration owns only the normalized declared path and whether acquisition is enabled. Do not duplicate this authority in Manager or Installer.

## Read first

- `AGENTS.md`
- repository read-first specs and registry
- `specs/configuration_contract_v1.yaml`
- issue #12 and comment `5172735375`
- `src/acquisition.mjs`
- `src/config.mjs`
- acquisition tests
- `vendor/background-service-runtime/native/CMakeLists.txt`
- `vendor/background-service-runtime/native/uci_arena_background_guard.cpp`
- native toolchain receipt/release files and tests

## Required architecture

Add one internal adapter, for example `withAcquisitionCredential(...)`, with a callback-scoped API. It must not return a reusable credential or cache bytes on an instance.

### Shared contract

- configured path is absolute, syntactically bounded, and contains no NUL/control ambiguity;
- every existing ancestor and the final object are admitted at use time;
- final object is a regular file, not a symlink, junction, reparse point, directory, socket, FIFO, device, or other special object;
- opened object remains physically outside package, repository, installed suite, workspace, and immutable component roots;
- pre-open path identity, opened-object identity, post-read identity, and post-callback path identity agree;
- read through the opened object only, at most 4,097 bytes, rejecting byte 4,097;
- reject mutation, growth, truncation, replacement, identity change, and material timestamp/size change during the operation;
- close on every path, preserving primary failure and a bounded internal close classification;
- externally expose only code `BOOK_FORGE_AUTH` and message `Lichess acquisition credential is unavailable`;
- never put credential path or bytes in logs, errors, telemetry, catalog, retry state, argv, environment, reusable headers, or committed fixtures;
- every retry reacquires the current exact file.

### Exact token byte grammar

Define it in a governing spec before implementation. Use exact ASCII bytes suitable for a bearer token:

- length 1..4096;
- allowed bytes only from a declared ASCII token alphabet that excludes space, quote, backslash, colon, semicolon, comma, CR, LF, NUL, all C0/C1 controls, DEL, and every non-ASCII byte;
- no trim, normalization, case fold, replacement decoding, or byte transformation;
- the exact admitted byte sequence becomes the bearer value only inside the callback.

### POSIX

- traverse/admit ancestors without following substituted links where available;
- open final file with no-follow semantics;
- compare `lstat`/descriptor `fstat`/post-use path identity using device+inode plus exact regular-file mode, size, and material time fields;
- loop complete bounded reads and reject short/truncated/growing objects conservatively;
- never inspect one object and unlink or act on another.

### Windows

Ordinary Node path inspection is insufficient for junction/reparse identity. Extend one existing provider-local native helper under an explicit versioned protocol.

The Windows helper must:

- open with `CreateFileW` using flags that admit the reparse point rather than following it until classified;
- reject any ancestor/final reparse point or unsupported object type;
- use `GetFileInformationByHandleEx`/equivalent volume serial + file ID identity and exact size/time data;
- canonicalize and prove the opened handle is outside forbidden roots;
- retain the verified credential handle while the Node callback performs the request;
- use bounded private IPC: send exact credential bytes over an inherited private pipe, wait for an explicit completion/abort message, recheck path/object identity, then close;
- never place token bytes in argv, environment, process command line, JSON diagnostics, stdout/stderr logs, or persistent files;
- fail closed if the native helper is unavailable or protocol/version/identity checks fail.

Do not implement a one-shot helper that inspects a path and then lets Node reopen/read it; that preserves the race.

## Request lifetime

In `fetchPosition()`:

1. acquire resource permission first;
2. immediately enter the credential callback;
3. construct/use the one request inside the callback;
4. ensure timeout, abort, HTTP failure, retry, and callback exceptions release the credential owner and native helper;
5. release all application references to credential bytes after callback completion.

Do not retain the token while unrelated preparation occurs.

## Tests

Use production code and cover at least:

- exact valid external file without terminal newline;
- leading/trailing space, tab, CR, LF, NUL, DEL, quote, backslash, colon, semicolon, comma, non-ASCII, invalid UTF-8, and 4097-byte input;
- direct workspace/package/suite location and physical aliases;
- initial final symlink, ancestor symlink, Windows junction/reparse point, directory, FIFO/socket/device, permission failure;
- path absent at configuration load then redirected before use;
- replacement before open, during read, after read, and during callback;
- same-size mutation, growth, truncation, identity/time-only changes;
- short reads and read failure;
- close failure preserving primary cause and generic external error;
- repeated requests proving no cache and current-file reacquisition;
- abort/retry and HTTP failure proving bytes do not survive in errors/catalog/state;
- native helper unavailable, malformed protocol, premature exit, and cancellation;
- package-installed Windows and Linux behavior with no silently skipped parity tests.

Correct existing fixtures: valid token files must be external to workspace and contain no newline. Existing workspace/newline fixtures become rejection tests.

## Validation

Run required preflight and exact Node >=24.15 tests. Build and test the native helper with strict warnings on Windows. Run `npm run check`, focused loopback HTTP tests, package-installed tests, and explicit Windows/Linux identity/race tests. Record exact commands and output; do not claim parity from source review or skipped tests.

## Delivery

Open a draft PR from `agent/exact-acquisition-credential-use` to `agent/config-contract-after-system-sanity`. Keep changes inside the acquisition credential owner, its native seam/protocol/build receipt, governing specs, and focused tests. Do not modify catalog, compiler/publication, Manager, Installer, or unrelated resource policy. Do not merge.
