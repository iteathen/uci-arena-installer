# Fresh package path selection

This current-generation compatibility route supports a fresh installation at an
explicit program and data location. It uses Windows Installer's existing
transaction ownership and does not implement the pending IL05/06/07 installed
descriptor or transaction-v2 architecture.

The fixed-scope suite bundle accepts persisted `ArenaProgramRoot` and
`ArenaDataRoot` variables. Manager MSI receives `INSTALL_ROOT` and
`ARENA_DATA_ROOT`, remembers both on repair, rejects conflicting replacement
locations before registry/file publication, and removes the selection on normal
uninstall. Normal uninstall preserves application data.

The outer Windows launcher selects the fixed-scope child. For a fresh unattended
personal installation, explicitly pass `--scope personal` before forwarded Burn
arguments such as `/install /quiet /norestart`. `/quiet` alone does not select
scope; without an existing installation or an explicit scope, the launcher waits
at its scope selector before staging a child or executing packages. An existing
installation retains its observed scope, and a conflicting override rejects.

MSI owns one four-string record in the 64-bit registry view:
`Software\UCI Arena\Installer\PackageLayout\<scope>`. It contains `Schema`,
`Scope`, `IntegrationRoot`, and `DataRoot`. Scope selects HKCU or HKLM. Root
readers normalize the one trailing separator Windows Installer adds to a
directory property, reject malformed/partial/opposite-scope records, and admit
all existing projected roots and ancestors through the no-reparse filesystem
adapter. No consumer writes this record separately.

`arena-setup --install-scope per-user --json layout` returns the closed
`arena_package_layout_selection_v1` projection: `scope`, `integration_root`,
and `roots` containing `data`, `install`, `cache`, `workspace`, and `knowledge`.
All relative children come from `packaging/install-layout.json`. The public
query must run through the selected installed runtime. Missing or invalid
selection returns exit 2 with `selection_unavailable` or `selection_invalid`.
It never returns guessed roots.

Manager obtains this public projection and passes the complete root tuple to
setup. The updater reads only the program-root launcher pointer, then obtains
the admitted roots from that same public command. Explicit roots must match an
existing selection. When selection is absent, mutating legacy/development CLI
invocations require all five explicit roots; read-only diagnostics retain their
historical behavior.

Focused coverage includes real native registry records in UUID test namespaces,
owned cache/installed and dangling junctions, deterministic Node/native child
projection, compiled MSIs/Burn bundles and a seven-case real Windows Installer
fixture covering repair, conflict rejection before writes, rollback and normal
uninstall. No fixed product registration, service, PATH or shortcut was altered
by that fixture.
