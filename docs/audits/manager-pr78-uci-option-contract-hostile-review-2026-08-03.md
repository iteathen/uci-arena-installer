# Manager PR #78 hostile review — pure UCI option contract

Date: 2026-08-03 America/Los_Angeles  
Repository: `iteathen/uci-arena-manager`  
PR: #78 `agent/uci-option-contract-v1`  
Reviewed head: `9e18b058b4870b02f65b08fadb8d6f14df85d97b`  
Base: Manager PR #54 / `agent/release-assembly` at `233654568e91d50eb5e0a995fd22e109c3d2a66d`  
Formal review: `4850726806`  
Disposition: **changes required; keep draft**

## Review result

The component is otherwise a strong pure brick: narrow ownership, explicit bounds, deterministic scalar admission, strict type grammar, overflow-safe cumulative accounting, and no filesystem/process/global mutable state. Two representation defects prevent its canonical output from being safely commandable.

## Confirmed blockers

1. **Option names may contain the `value` command delimiter.** A line such as `option name Mode value Choice type string default x` is accepted. Downstream construction becomes `setoption name Mode value Choice value y`, which cannot uniquely preserve the option-name/value boundary.
2. **String overrides admit leading/trailing ASCII spaces.** Advertised defaults reject edge spaces, but requested string values do not. The canonical map can therefore contain values that the plain UCI `setoption` command surface cannot represent with stable identity.
3. **Allocation evidence is not reproducible from tracked artifacts.** Issue #76 and the registered spec require representative 256-option allocation reporting. The PR body states 522 allocations, but no committed harness, receipt, or repeatable command establishes that result.
4. **Exact boundary tests are incomplete.** Missing focused evidence includes line-byte, identity-byte/cumulative, option name/value/variant, exact option/variant cardinality, command delimiter, string edge-space, and complete malformed UTF-8 structural-class boundaries.

## Required correction

- define and enforce a command-safe option-name grammar, at minimum rejecting `value` as an ASCII-space-delimited name token;
- reject string override edge spaces under the current unescaped command design;
- add tracked exact-boundary tests;
- add a standalone production-parser allocation probe or bound receipt with toolchain identity.

## Audit consequence

PR #78 is not yet acceptance evidence. PR #79 and PR #80 must remain blocked until the canonical option representation is guaranteed to round-trip exactly through the eventual UCI command boundary.