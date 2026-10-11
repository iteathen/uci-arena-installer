# Historical UCI PR dispositions

Reviewed 2026-10-11 against accepted producer main, using complete changed-file
diffs and current contracts. Squash histories are not treated as evidence that
unmerged work is absent or integrated. These are cleanup dispositions, not new
native acceptance of old heads. Remote closure/branch cleanup is a separate
operator action; retained artifacts and occupied/dirty worktrees are preserved.

Installer142/145/150 and legacy Engine136/145 were subsequently closed by the
operator after exact-head checks; their public metadata/diffs remain retained.
Installer140 awaits publication of its unique-coverage ledger. Unique unfinished
Installer143/144/148 remain open. The governance reconciliations below are source
review candidates until their respective producer commits are accepted.

The operator subsequently retired all 126 approved stale refs after the bounded
review. The retained retirement completion packet SHA-256 is
`3a9b6e6fdb73a9e4aaaedf5a97adbd0dac6f649594aa1d6a5d614f6e6ff6e92f`.
Approved stale refs are distinct from unfinished PRs, retained worktrees,
source-data backups and qualification artifacts; this does not turn unfinished
143/144/148 into accepted or integrated work.

| Repository / old PR | Exact old head | Disposition and retained work |
| --- | --- | --- |
| Installer [140](https://github.com/iteathen/uci-arena-installer/pull/140) | `c47c4b989bdc510ce17963741cdb33f119da34ba` | Windows identity owner replaced by #174 (`07264962250b15491d8d78127841b454d1151f0b`); preserve unique edge coverage listed below. |
| Installer [142](https://github.com/iteathen/uci-arena-installer/pull/142) | `16e5f377c5132baa814106d391aa964952f61cb3` | ExactDocumentReader/bounded JSON replaced by #175 (`2e822e61cd1579848f4f6cde09fe6eeda03e6ecc`), with current bounds and tests. |
| Installer [143](https://github.com/iteathen/uci-arena-installer/pull/143) | `ea98fc68a716198b6e6408859fc708e0c29de242` | Retain unfinished Linux FilesystemIdentity adapter; exact-head Linux native qualification remains required. |
| Installer [144](https://github.com/iteathen/uci-arena-installer/pull/144) | `a22ef27dc4a8e444ec22c4b001dcacc132e411e3` | Retain unique common ArtifactAcquirer/cache draft; reconcile current ports and qualify native adapter/integration closure. |
| Installer [145](https://github.com/iteathen/uci-arena-installer/pull/145) | `e9f927353382364ce016afb19a427f3f7483161f` | Historical Sol/Spark relay experiment; archive rather than install/run as product cleanup. |
| Installer [148](https://github.com/iteathen/uci-arena-installer/pull/148) | `982a2d6186d5ccdeb948a59120fc529bae7afad3` | Retain unique ArchiveExpander/extraction port draft. Old tests are historical; current-main reconciliation, native adapter and fresh qualification remain. |
| Installer [150](https://github.com/iteathen/uci-arena-installer/pull/150) | `16481d97605e6bad5375676528c287cd1a250e33` | Neutral ServiceControl replaced by #177 (`27ac9805832888480bd1cfece0755c95007bad35`); neutral port header is byte-identical. |
| Installer [160](https://github.com/iteathen/uci-arena-installer/pull/160) | `2636df3dad0dc07a5996ef7249ad256a5b5db601` | Unique move-time governance reconciled in the companion document; current Vector and public consumer-clock contracts control implementation. |
| Installer [165](https://github.com/iteathen/uci-arena-installer/pull/165) | `e44d217d3238854f5c484339ed041a076078d192` | Unique strong-immutability direction reconciled as target acceptance, not already-implemented architecture. |
| Manager [124](https://github.com/iteathen/uci-arena-manager/pull/124) | `16dcf77e9bd720364570e06e9a6e05db653cc4a8` | Reconcile producer-owned projection spec under user-accepted SP-004; obsolete blanket clock-transit ban does not override current 0.3.29 behavior. |
| Evidence [47](https://github.com/iteathen/uci-arena-evidence-service/pull/47) | `ec3e58407d6ad218cbda2d81e94ea41730cd88c5` | Reconcile producer-owned population-policy purpose with current 0.3.19 runtime-v2, exact clocks and qualification limits. |
| Legacy Engine [136](https://github.com/iteathen/uci-arena-engine/pull/136) | `a0e4474b420344e6e45fdb96460bec59d7a5c065` | Retire cancelled campaign cursor/legacy qualification replacement; retain historical generic semantics and evidence. |
| Legacy Engine [145](https://github.com/iteathen/uci-arena-engine/pull/145) | `b8849cb6da75e4a218f66635c98809174346368e` | Retire cancelled C++ graph/search implementation; historical native results do not qualify Vector or clear old performance/strength gates. |

## FI140 remaining review and coverage

PR140 is not byte-equivalent to the replacement and its exact last head lacked
completed native acceptance. Its remaining details must not disappear merely
because the foundational owner was replaced. These are coverage/review requests,
not a statement that current main implements or fails them:

- Ordinary UNC traversal begins at the share root, never a server-only pseudo-root.
- Already-extended drive, UNC and volume-GUID paths begin at their actual physical
  root, preserving capability reads/binding for already-extended input.
- Malformed/unsupported extended roots and object namespaces such as GLOBALROOT,
  pipe/mailslot namespaces and extended UNC pipe paths fail through the declared
  neutral special-object/invalid-path boundary before ordinary traversal.
- Deterministic ancestor FileId mismatch verifies the retained full-chain binding;
  use a private test seam instead of a Windows-invalid live directory-rename premise.
- Success-path capability lifetime and failure cleanup retain native handles only
  for their owned lifetime; same-size mutation and explicit 64-bit read/EOF cases
  remain separately observable.

Any new test or correction belongs to the current FilesystemIdentity owner and
requires fresh platform evidence. Do not merge the obsolete alternative adapter
over the accepted one just to recover these cases. The generic Installer/native
helper work is independent of the cancelled C++ chess/search campaign.

## Provenance and qualification boundary

The retained external audit packet is `disposition-review.json`, SHA-256
`c3889565197b6ddbf38ff5281cfa06561695e0a1b076919afd83d3efbc8d7899`, with
13 exact repo/head/base rows, full public diffs and 92 verified artifact hashes.
It was source/contract review only: no PR execution, source/remote mutation,
deployment, Task or credential effects. These reconciliation source commits
require their own spec validation and independent review before replacement
drafts are merged or closed. Original C backups and release/acceptance artifacts
are outside stale-branch cleanup.
