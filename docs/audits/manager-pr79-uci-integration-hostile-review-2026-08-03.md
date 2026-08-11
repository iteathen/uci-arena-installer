# Manager PR #79 hostile review — UCI option-contract process integration

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-manager`  
PR: #79 `agent/integrate-uci-option-contract`  
Reviewed head: `0cee8727caab3c9d6c44ce8dd4a4012f677131ba`  
Base: PR #78 / `agent/uci-option-contract-v1` at `9e18b058b4870b02f65b08fadb8d6f14df85d97b`  
Formal review: `4850708644`  
Disposition: **changes required; keep draft**

## Confirmed blockers

1. `await_token()` accepts `uciok trailing` and `readyok trailing`; only bestmove is value-bearing.
2. Pre-option runtime model identity remains in the result when an option-selected deployment does not republish identity after options, contradicting the product contract.
3. Repeated/conflicting valid runtime identity silently overwrites prior authority; recognized malformed announcements disappear as absence.
4. Provider-token validation uses locale-sensitive `std::isalnum` byte classification rather than an exact ASCII grammar.
5. Compatibility diagnostics match one exact exception string and rebuild folded option-name authority outside the pure contract.
6. No registered integration spec owns UCI interrogation phases, token grammar, effective runtime identity, option command projection, and error semantics; PR #78’s spec explicitly excludes UciProcess integration.
7. The claimed 4096-byte acquisition edge occurs after the existing NativeProcess has materialized a complete line under its larger raw-line bound. The branch proves metadata-storage bounds, not raw child-output framing.
8. Process tests omit exact/ambiguous readiness tokens, runtime identity phase/cardinality behavior, locale-independent provider identity, and typed compatibility error projection.

## Required correction

- exact uciok/readyok and separate bestmove grammar;
- explicit pre-option versus effective post-option runtime identity phases;
- exact duplicate/conflict/malformed identity handling;
- one deterministic ASCII/UTF-8 token owner;
- typed contract errors rather than exception-prose matching;
- a registered UCI probe integration specification;
- truthful separation of metadata storage from raw process framing evidence.

## Audit consequence

PR #79 is not acceptance evidence. PR #80 must not be corrected or rebased as final framing ancestry until PR #79’s process integration and identity semantics are repaired.