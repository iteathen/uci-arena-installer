# Fresh package path selection

This current-generation compatibility route supports a fresh installation at an
explicit program and data location. It uses Windows Installer's existing
transaction ownership and does not implement the pending IL05/06/07 installed
descriptor or transaction-v2 architecture.

The per-user permission amendment follows the accepted SP-001 operator policy:
refuse an unsafe existing folder and explain explicit preparation or a fresh
location. Its source implementation and Windows fixture evidence are reviewable
for the 0.3.60 candidate; it is not an existing 0.3.59 packaging guarantee. The accepted 0.3.59
E installation required explicit operator preparation of the selected root after
Manager correctly refused inherited broad write authority. That operational
record is retained separately; it does not authorize automatic ACL adoption.
The 0.3.60 composition requires Installer runtime 0.2.3 for this repair; all eight
component versions and their signed inputs remain the same as 0.3.59.

Under this Personal-installation policy, the selected data root must have ordinary existing
ancestors. Setup creates one missing final data directory with a protected DACL
already in place: the interactive user, LocalSystem, and Administrators may
write; built-in users may read and traverse. An inherited broad parent permission
does not become write authority on this new root or its newly created state
directories. Missing parents, links, junctions, and unsafe existing roots fail
before SetupEngine's component/data-state effects. The current Burn chain may
already have applied Manager MSI and rolls it back on a later refusal; this is
not a zero-effects guarantee across the whole bootstrapper.

A valid TUF catalog load can publish anti-rollback security state during `plan`
or unconfirmed `apply`. The resolved per-user root is transferred explicitly to
that publication owner and prepared before the state write. Signature, expiry,
version and rollback validation remain mandatory. Exact no-reparse reading and
durable publication protect the state target; unsafe retained state-directory
permissions are refused rather than changed. Invalid TUF inputs and nonpublishing
unsigned catalog diagnostics do not create a fresh data root merely to inspect
the catalog. The signed/TUF fixture qualifies this path separately from the
installed 0.3.59 unsigned development suite.
Root preparation follows TUF signature, expiry, and delegated-version checks;
rollback-version checks still precede state replacement. Catalog schema
validation occurs later, so a valid signed target with an unsupported catalog
schema may legitimately leave private rollback-protection state before failing.

An existing data directory is inspected, never silently repaired. If setup
reports that the per-user data root is not private, choose a fresh data location
or explicitly prepare the exact selected directory's permissions as its owner.
Preserve its prior security metadata for recovery; permit write authority only
for the owning interactive user, LocalSystem, and Administrators, and retain
read/traverse access for built-in users if required. This is an operator security
operation, separate from installer confirmation. Do not recursively replace
retained file ACLs, read or copy client keys, change the source backup, or weaken
Manager's key admission. Setup does not certify the permissions of retained
children; Manager still admits its own key through its native security port.

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
