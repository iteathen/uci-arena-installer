# JS suite 0.3.52 composition

Status: composition and Windows feature policy; release qualification pending.

The fresh composition is `compositions/complete-arena-0.3.52.json`. It is product
data for the existing generic Installer 0.2.1 runtime. It is not a release lock,
signed catalog, installation receipt, or permission to start a credential-gated
Bot. Historical compositions and their artifacts remain separately versioned.

Windows Install and Modify expose Vector, Book Forge, the root tablebase
provider, Evidence Service and Lichess Bot. All five are selected initially;
each is optional as an operator root. The catalog retains Manager as the base
component and derives dependencies from the composition and verified manifests.
An unchecked Vector checkbox cannot remove Vector while Evidence still needs it.

| Component | Exact version | Required dependencies |
| --- | --- | --- |
| Manager | 0.3.26 | none |
| Vector | 0.1.0 | private Node 26.11.1 |
| Book Forge | 0.4.8 | private Node 26.11.1, Stockfish 15.1 |
| Evidence Service | 0.3.15 | Vector, private Node 26.11.1 |
| Lichess Bot | 1.0.41 | private Node 26.11.1 |
| Root provider install component | 2.1.1 | none |

Stockfish is the Book Forge builder and an independent reference. It is not the
default engine or a Vector fallback. Vector's public launch profile is the
selected default for this composition. Bot consumes it through its declared
optional binding; the Bot's required dependency list does not force Vector.
Manager's public engine candidate, probe, register and refresh operations remain
the engine replacement path. This data does not hardcode an engine into Manager.

The root provider is optional to Vector. Its declared component-path binding
uses the `runtime` subtree with `required: false`; clearing its checkbox is
supported. Install version 2.1.1 wraps the unchanged inner provider contract
2.1.0. This profile bundles neither Syzygy tables nor a published opening book,
and does not claim full-six ranking qualification. Book Forge is a producer,
not a substitute for a published book artifact.

Scope roots, elevation and upgrade identities are unchanged. No native setup
mechanism or provider behavior changes. Synthetic catalog tests validate data
projection only; WiX build tests do not install. A release still requires actual
signed component rows, complete provenance, inventory, compatibility and native
qualification before a separate reviewed setup transaction.
