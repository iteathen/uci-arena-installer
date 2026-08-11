[CmdletBinding()]
param(
    [ValidateSet('auto', 'per-user', 'per-machine')]
    [string]$Scope = 'auto',
    [string]$ManagerProgramRoot = '',
    [string]$DataRoot = '',
    [string]$InstallRoot = '',
    [string]$CacheRoot = '',
    [string]$KnowledgeRoot = ''
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

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifestPath = Join-Path $packageRoot 'update-package.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'The suite update package manifest is unavailable.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -Depth 32
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
        $digest = (Get-FileHash -LiteralPath $filename -Algorithm SHA256).Hash.ToLowerInvariant()
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
if ($Scope -in @('auto', 'per-user')) {
    $scopeCandidates += [pscustomobject]@{
        Scope = 'per-user'
        Program = if ($ManagerProgramRoot) { $ManagerProgramRoot } else {
            Join-Path $env:LOCALAPPDATA 'Programs\UCI Arena Manager'
        }
        Data = if ($DataRoot) { $DataRoot } else {
            Join-Path $env:LOCALAPPDATA 'UCI Arena Manager'
        }
    }
}
if ($Scope -in @('auto', 'per-machine')) {
    $scopeCandidates += [pscustomobject]@{
        Scope = 'per-machine'
        Program = if ($ManagerProgramRoot) { $ManagerProgramRoot } else {
            Join-Path $env:ProgramFiles 'UCI Arena Manager'
        }
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
    ConvertFrom-Json -Depth 16
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
$catalog = Join-Path $packageRoot 'release\catalog.json'
$arguments = @(
    '--catalog', $catalog,
    '--install-scope', $selected.Scope,
    '--install-root', $effectiveInstallRoot,
    '--data-root', $effectiveDataRoot,
    '--cache-root', $effectiveCacheRoot,
    '--knowledge-root', $effectiveKnowledgeRoot,
    '--json'
)
if ($manifest.trust -eq 'tuf') {
    $arguments += @('--metadata-dir', (Join-Path $packageRoot 'release\tuf'))
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
try {
    $arguments = @('--result-file', $resultPath) + $arguments
    & $setup @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "arena-setup failed with exit code $LASTEXITCODE."
    }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -Depth 64
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
    Remove-Item -LiteralPath $resultPath -Force -ErrorAction SilentlyContinue
}
