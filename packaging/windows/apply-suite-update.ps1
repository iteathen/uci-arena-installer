[CmdletBinding()]
param(
    [ValidateSet('auto', 'per-user', 'per-machine')]
    [string]$Scope = 'auto',
    [string]$ManagerProgramRoot = '',
    [string]$DataRoot = '',
    [string]$InstallRoot = '',
    [string]$CacheRoot = '',
    [string]$WorkspaceRoot = '',
    [string]$KnowledgeRoot = '',
    [switch]$ValidateMediaOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Test-SemanticVersionAtLeast {
    param([string]$Actual, [string]$Minimum)
    $actualParts = $Actual.Split('.')
    $minimumParts = $Minimum.Split('.')
    if ($actualParts.Count -ne 3 -or $minimumParts.Count -ne 3) { return $false }
    for ($index = 0; $index -lt 3; $index += 1) {
        [int]$actualValue = 0
        [int]$minimumValue = 0
        if (-not [int]::TryParse($actualParts[$index], [ref]$actualValue) -or
            -not [int]::TryParse($minimumParts[$index], [ref]$minimumValue)) {
            return $false
        }
        if ($actualValue -gt $minimumValue) { return $true }
        if ($actualValue -lt $minimumValue) { return $false }
    }
    return $true
}

function Get-Sha256Hex {
    param([string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Test-ExactArtifactFile {
    param([string]$Path, [pscustomobject]$Artifact)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.Length -ne [int64]$Artifact.size_bytes -or
        [string]$Artifact.sha256 -notmatch '^[0-9a-f]{64}$') {
        return $false
    }
    return (Get-Sha256Hex -Path $Path) -eq [string]$Artifact.sha256
}

function Get-PlatformArtifact {
    param([pscustomobject]$Component)
    $property = $Component.artifacts.PSObject.Properties['windows-x86_64']
    if ($null -eq $property) {
        $property = $Component.artifacts.PSObject.Properties['any']
    }
    if ($null -eq $property) {
        throw "Catalog component $($Component.id) has no Windows or portable artifact."
    }
    return $property.Value
}

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $packageRoot 'update-package.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'The suite update package manifest is unavailable.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 'arena_suite_update_package_v1' -or
    $manifest.schema_version -ne 1 -or
    $manifest.target_platform -ne 'windows-x86_64') {
    throw 'The suite update package contract is unsupported.'
}

$declaredPaths = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($file in $manifest.files) {
    if ($file.path -notmatch '^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$') {
        throw "The update package declares an unsafe path: $($file.path)"
    }
    if (-not $declaredPaths.Add([string]$file.path)) {
        throw "The update package repeats a path: $($file.path)"
    }
    $filename = Join-Path $packageRoot ($file.path -replace '/', '\')
    $item = Get-Item -LiteralPath $filename -Force
    if (-not $item.PSIsContainer -and $item.Length -eq [int64]$file.size_bytes) {
        $digest = Get-Sha256Hex -Path $filename
        if ($digest -eq [string]$file.sha256) { continue }
    }
    throw "The update package file failed verification: $($file.path)"
}
$actualFiles = Get-ChildItem -LiteralPath $packageRoot -File -Recurse -Force |
    ForEach-Object {
        $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
    } | Where-Object { $_ -ne 'update-package.json' }
foreach ($relative in $actualFiles) {
    if (-not $declaredPaths.Contains($relative)) {
        throw "The update package contains an undeclared file: $relative"
    }
}
if ($actualFiles.Count -ne $declaredPaths.Count) {
    throw 'The update package file inventory is incomplete.'
}

$scopeCandidates = @()
function Get-PackageProgramPointer {
    param([string]$RequestedScope)
    $hive = if ($RequestedScope -eq 'per-user') { [Microsoft.Win32.RegistryHive]::CurrentUser } else { [Microsoft.Win32.RegistryHive]::LocalMachine }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $null
    try {
        $key = $base.OpenSubKey("Software\UCI Arena\Installer\PackageLayout\$RequestedScope", $false)
        if ($null -eq $key) { return '' }
        if ($key.GetValueKind('IntegrationRoot') -ne [Microsoft.Win32.RegistryValueKind]::String) {
            throw 'The installed program locator is invalid.'
        }
        $pointer = [string]$key.GetValue('IntegrationRoot', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if (-not $pointer -or $pointer.Length -gt 32767 -or -not [System.IO.Path]::IsPathRooted($pointer)) {
            throw 'The installed program locator is invalid.'
        }
        return $pointer
    } finally {
        if ($null -ne $key) { $key.Dispose() }
        $base.Dispose()
    }
}
if ($Scope -in @('auto', 'per-user')) {
    $personalPointer = Get-PackageProgramPointer -RequestedScope 'per-user'
    $scopeCandidates += [pscustomobject]@{
        Scope = 'per-user'
        Program = if ($ManagerProgramRoot) { $ManagerProgramRoot } elseif ($personalPointer) { $personalPointer } else {
            Join-Path $env:LOCALAPPDATA 'Programs\UCI Arena Manager'
        }
        Data = if ($DataRoot) { $DataRoot } else {
            Join-Path $env:LOCALAPPDATA 'UCI Arena Manager'
        }
        Selected = [bool]$personalPointer
    }
}
if ($Scope -in @('auto', 'per-machine')) {
    $machinePointer = Get-PackageProgramPointer -RequestedScope 'per-machine'
    $scopeCandidates += [pscustomobject]@{
        Scope = 'per-machine'
        Program = if ($ManagerProgramRoot) { $ManagerProgramRoot } elseif ($machinePointer) { $machinePointer } else {
            Join-Path $env:ProgramFiles 'UCI Arena Manager'
        }
        Selected = [bool]$machinePointer
        Data = if ($DataRoot) { $DataRoot } else {
            Join-Path $env:ProgramData 'UCI Arena Manager'
        }
    }
}
$installed = @($scopeCandidates | Where-Object {
    Test-Path -LiteralPath (Join-Path $_.Program 'bin\arena-setup.exe') -PathType Leaf
})
if ($installed.Count -ne 1) {
    throw "Expected exactly one installed UCI Arena scope, found $($installed.Count)."
}
$selected = $installed[0]
$setup = Join-Path $selected.Program 'bin\arena-setup.exe'
$runtimeManifestPath = Join-Path $selected.Program 'libexec\uci-arena-installer\arena-installer-runtime.json'
$runtimeManifest = Get-Content -LiteralPath $runtimeManifestPath -Raw |
    ConvertFrom-Json
if ($runtimeManifest.schema -ne 'arena_installer_runtime_v1' -or
    -not (Test-SemanticVersionAtLeast -Actual $runtimeManifest.runtime_version `
        -Minimum $manifest.installer_minimum_version)) {
    throw "Installed arena-setup runtime $($runtimeManifest.runtime_version) is older than required $($manifest.installer_minimum_version)."
}

$effectiveDataRoot = $selected.Data
$effectiveInstallRoot = if ($InstallRoot) { $InstallRoot } else {
    Join-Path $effectiveDataRoot 'installed'
}
$effectiveCacheRoot = if ($CacheRoot) { $CacheRoot } else {
    Join-Path $effectiveDataRoot 'cache'
}
$effectiveKnowledgeRoot = if ($KnowledgeRoot) { $KnowledgeRoot } else {
    Join-Path $effectiveDataRoot 'knowledge'
}
$effectiveWorkspaceRoot = if ($WorkspaceRoot) { $WorkspaceRoot } else {
    Join-Path $effectiveDataRoot 'workspaces'
}
if ($selected.Selected) {
    $layoutText = & $setup --install-scope $selected.Scope --json layout
    if ($LASTEXITCODE -ne 0) { throw 'The installed path authority could not be admitted.' }
    $layout = ($layoutText -join "`n") | ConvertFrom-Json
    if ($layout.schema -ne 'arena_package_layout_selection_v1' -or $layout.scope -ne $selected.Scope -or
        [System.IO.Path]::GetFullPath([string]$layout.integration_root).TrimEnd('\') -ine [System.IO.Path]::GetFullPath($selected.Program).TrimEnd('\')) {
        throw 'The installed path authority does not match the selected program.'
    }
    foreach ($pair in @(@($DataRoot, $layout.roots.data), @($InstallRoot, $layout.roots.install),
        @($CacheRoot, $layout.roots.cache), @($WorkspaceRoot, $layout.roots.workspace), @($KnowledgeRoot, $layout.roots.knowledge))) {
        if ($pair[0] -and [System.IO.Path]::GetFullPath([string]$pair[0]).TrimEnd('\') -ine [System.IO.Path]::GetFullPath([string]$pair[1]).TrimEnd('\')) {
            throw 'Update overrides conflict with the installed path selection.'
        }
    }
    $effectiveDataRoot = [string]$layout.roots.data
    $effectiveInstallRoot = [string]$layout.roots.install
    $effectiveCacheRoot = [string]$layout.roots.cache
    $effectiveWorkspaceRoot = [string]$layout.roots.workspace
    $effectiveKnowledgeRoot = [string]$layout.roots.knowledge
}
$packageReleaseRoot = Join-Path $packageRoot 'release'
$packageCatalog = Join-Path $packageReleaseRoot 'catalog.json'
$catalogDocument = Get-Content -LiteralPath $packageCatalog -Raw |
    ConvertFrom-Json
if ($catalogDocument.schema -ne 'arena_install_catalog_v1' -or
    $catalogDocument.schema_version -ne 1 -or
    $catalogDocument.suite_version -ne $manifest.suite_version) {
    throw 'The update package catalog identity is invalid.'
}
$closureIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($componentId in @($manifest.dependency_closure)) {
    if ([string]$componentId -notmatch '^[A-Za-z0-9_.-]+$' -or
        -not $closureIds.Add([string]$componentId)) {
        throw 'The update package dependency closure is invalid.'
    }
}
if ($closureIds.Count -lt 1) {
    throw 'The update package dependency closure is empty.'
}
foreach ($componentId in @($manifest.selected_components)) {
    if (-not $closureIds.Contains([string]$componentId)) {
        throw 'The selected components are not contained by the dependency closure.'
    }
}
$catalogIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($component in @($catalogDocument.components)) {
    if ([string]$component.id -notmatch '^[A-Za-z0-9_.-]+$' -or
        -not $catalogIds.Add([string]$component.id)) {
        throw 'The update package catalog component identity is invalid.'
    }
}
foreach ($componentId in $closureIds) {
    if (-not $catalogIds.Contains($componentId)) {
        throw "The update package catalog omits closure component $componentId."
    }
}

$normalizedDataRoot = [System.IO.Path]::GetFullPath($effectiveDataRoot)
$materializedMediaRoot = Join-Path $normalizedDataRoot `
    ("routine-update-media-{0}" -f [guid]::NewGuid().ToString('N'))
$resultPath = ''
try {
    New-Item -ItemType Directory -Path $materializedMediaRoot | Out-Null
    Copy-Item -LiteralPath $packageCatalog `
        -Destination (Join-Path $materializedMediaRoot 'catalog.json')
    $materializedSources = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    [int]$packagedPayloads = 0
    [int]$retainedPayloads = 0
    foreach ($component in @($catalogDocument.components)) {
        $artifact = Get-PlatformArtifact -Component $component
        $source = [string]$artifact.source
        if ($source.StartsWith('https://', [System.StringComparison]::OrdinalIgnoreCase)) {
            if ($closureIds.Contains([string]$component.id)) {
                throw "Closure component $($component.id) does not have a packaged artifact."
            }
            continue
        }
        if ($source -notmatch '^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$' -or
            -not $materializedSources.Add($source)) {
            throw "The update catalog artifact source is unsafe or repeated: $source"
        }
        $isClosureArtifact = $closureIds.Contains([string]$component.id)
        $sourceFile = if ($isClosureArtifact) {
            Join-Path $packageReleaseRoot ($source -replace '/', '\')
        } else {
            Join-Path (Join-Path $normalizedDataRoot 'release') ($source -replace '/', '\')
        }
        if (-not (Test-ExactArtifactFile -Path $sourceFile -Artifact $artifact)) {
            $authority = if ($isClosureArtifact) { 'package' } else { 'retained release channel' }
            throw "Catalog artifact is unavailable from its exact $authority authority: $source"
        }
        $destination = Join-Path $materializedMediaRoot ($source -replace '/', '\')
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force |
            Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $destination
        if ($isClosureArtifact) { $packagedPayloads += 1 } else { $retainedPayloads += 1 }
    }

    if ($ValidateMediaOnly) {
        [pscustomobject]@{
            schema = 'arena_suite_update_media_validation_v1'
            state = 'validated'
            suite_version = $manifest.suite_version
            catalog_components = $catalogIds.Count
            packaged_payloads = $packagedPayloads
            retained_payloads = $retainedPayloads
        } | ConvertTo-Json -Compress
        return
    }

    $catalog = Join-Path $materializedMediaRoot 'catalog.json'
    $arguments = @(
        '--catalog', $catalog,
        '--install-scope', $selected.Scope,
        '--install-root', $effectiveInstallRoot,
        '--data-root', $effectiveDataRoot,
        '--cache-root', $effectiveCacheRoot,
        '--workspace-root', $effectiveWorkspaceRoot,
        '--knowledge-root', $effectiveKnowledgeRoot,
        '--json'
    )
    if ($manifest.trust -eq 'tuf') {
        $arguments += @('--metadata-dir', (Join-Path $packageReleaseRoot 'tuf'))
    } elseif ($manifest.trust -eq 'unsigned_development') {
        $arguments += '--allow-unsigned-development-catalog'
    } else {
        throw 'The suite update package trust mode is unsupported.'
    }
    if ($selected.Scope -eq 'per-machine') {
        $arguments += '--restart-manager-service'
    }
    $arguments += @('upgrade', '--preset', 'custom', '--offline', '--yes')
    foreach ($component in $manifest.selected_components) {
        $arguments += @('--component', [string]$component)
    }

    $resultPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("uci-arena-update-result-{0}.json" -f [guid]::NewGuid().ToString('N'))
    $arguments = @('--result-file', $resultPath) + $arguments
    & $setup @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "arena-setup failed with exit code $LASTEXITCODE."
    }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($result.state -ne 'committed') {
        throw "arena-setup did not commit the update: $($result.state)"
    }
    [pscustomobject]@{
        schema = 'arena_suite_update_apply_result_v1'
        state = 'committed'
        suite_version = $manifest.suite_version
        scope = $selected.Scope
        selected_components = @($manifest.selected_components).Count
        dependency_components = @($manifest.dependency_closure).Count
        manager_restart = if ($selected.Scope -eq 'per-machine') { 'completed' } else { 'required' }
    } | ConvertTo-Json -Compress
} finally {
    if ($resultPath) {
        Remove-Item -LiteralPath $resultPath -Force -ErrorAction SilentlyContinue
    }
    if ((Split-Path -Parent $materializedMediaRoot) -eq $normalizedDataRoot -and
        (Split-Path -Leaf $materializedMediaRoot) -match '^routine-update-media-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $materializedMediaRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
