# Related bundle upgrade authority

The current complete-suite component transaction stages, adopts, replaces and
rolls back selected components. A historical Burn bundle is not a second
component lifecycle authority. Its registered executor may implement an older
receipt or provider contract, even when its executable remains cached.

The fixed-scope BAFunctions adapter therefore sets the plan relation to `None`
for every related `Upgrade` occurrence and defensively requests `None` from
both execute and restore callbacks for those exact identities. Genuine
`Downgrade` detection and independent addon/patch plan types remain unchanged.
No product name, source version, cache filename or registry contents selects
this behavior. Historical bundle registrations remain retained and **unretired**.
This repair does not claim to uninstall them and does not clean the registry.

WiX 6.0.2 plans restoration separately: when a current package installs, a
related `Upgrade` gets a default `ForcePresent` restore request even if its
execute request is `None`. Suppressing only duplicate execute requests does
not prevent rollback from starting a historical executor. The prior guard
also allowed the first cached occurrence of each old identity to execute.

The regression test loads the actual built DLL and calls its public
`BAFunctionsCreate` callback using the locked WiX 6.0.2 SDK headers. It covers
repeated identities, independent restoration, missing cache, reset boundaries,
downgrade/addon/patch preservation, malformed arguments, and theme loading.
These are callback-boundary tests; they do not claim an installed upgrade ran.
The existing opt-in owned upgrade-cycle fixture now requires no old execute,
rollback or restore plan. It must not be run against a user's installation.

Upstream primary references:

- [WiX 6.0.2 planning](https://github.com/wixtoolset/wix/blob/v6.0.2/src/burn/engine/plan.cpp)
- [WiX 6.0.2 standard BA callbacks](https://github.com/wixtoolset/wix/blob/v6.0.2/src/ext/Bal/stdbas/WixStandardBootstrapperApplication.cpp)

The mandatory theme-loaded callback returns success without UI customization.
WiX treats `E_NOTIMPL` there as an error, while optional callbacks may use it
to select the existing fallback. The adapter does not replace the standard UI.
