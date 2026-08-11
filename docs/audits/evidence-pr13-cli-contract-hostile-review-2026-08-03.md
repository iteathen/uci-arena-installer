# Evidence PR #13 hostile review — strict CLI contract

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-evidence-service`  
PR: #13 `agent/strict-cli-contract-after-system-sanity`  
Reviewed head: `7b5e7f90476e17e5157f4a782a27122aa210a90b`  
Base: PR #10 / `agent/system-sanity` at `b09abb9aecf6aba6ca04358c8789eef86ecb763b`  
Formal review: `4851148254`  
Disposition: **changes required; reconstruction required; keep draft**

## Review result

Rejecting unknown syntax, duplicate flags, prefix integers, and top-level non-object JSON is useful. The current boundary remains a mutable string/flag bag with duplicated authority, generic accidental limits, lossy JSON admission, unversioned public behavior changes, unbounded diagnostics, and only self-referential parser tests.

## Confirmed blockers

1. Command paths, flags, required values, and enums are independently duplicated across YAML, `COMMANDS`, usage text, dispatcher branches, tests, and broader contracts.
2. Preflight only reports a spec pathname and does not mechanically verify registry/spec/usage/dispatcher parity.
3. The parser returns mutable `{command, globals, flags}` bags rather than one versioned discriminated project-owned command contract.
4. Generic 65,536-byte token, 1,048,576-byte invocation, and `-1_000_000..1_000_000` priority limits are not domain-derived and do not define exact bounds for each ID, path, string, list, identity, timestamp, diagnostic, or allocation consequence.
5. `JSON.parse` admits duplicate member names, unsafe/lossy numbers, excessive nesting/member counts, and unbounded nested strings/lists within the byte cap before values are fingerprinted or persisted.
6. Job states, export formats, campaign mode, and API control actions are copied into the parser instead of imported from one domain owner.
7. Package version remains `0.3.9` although duplicate, equals-form, flag-shaped-value, unknown-syntax, JSON, and integer behavior changes incompatibly.
8. Required values are byte-nonempty only; whitespace/control/newline-bearing strings and command-inappropriate lexical forms pass syntax admission.
9. Error prose can echo nearly the full input budget and embedded control characters; no typed code, field, byte cap, or bounded cause exists.
10. Integer syntax still admits noncanonical spellings such as leading zeros and `-0`; exported helper options can create unregistered semantics.
11. Parser helper exports allow callers to choose arbitrary defaults/bounds outside the registered command grammar.
12. Tests compare implementation-maintained registries with implementation-maintained fixtures, not the YAML or dispatcher.
13. No production CLI spawn proves real help side-effect behavior, dispatcher reachability, package/bin behavior, or Node 24.15 Windows/Linux argv/path semantics.
14. Downstream semantic admission is missing at several paths even though the spec claims syntax/domain separation.

## Required reconstruction boundary

- Establish one versioned command registry/discriminated command union and generate or mechanically validate YAML, usage, parser, dispatcher coverage, and tests.
- Import domain vocabularies and parsers from their owning contracts.
- Define exact command-field bounds, canonicalization, and bounded typed diagnostics.
- Replace generic JSON-object parsing with command-owned schemas that reject duplicate keys, unsafe numbers, excessive complexity, and ambiguous authority.
- Version and document the CLI compatibility/migration policy.
- Test the real executable and dispatcher on required Node 24.15 Windows and Linux environments.

## Audit consequence

PR #13 is not acceptance evidence. PR #14 and all later Evidence branches remain parent-blocked. PR #13 is independently parent-blocked by PR #10.