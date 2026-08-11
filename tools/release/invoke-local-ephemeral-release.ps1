[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryPath,
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Ref,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$ComponentId,
    [string]$Workflow = 'component-release.yml',
    [string]$Target = 'windows-x86_64',
    [string]$WorkflowComponent = '',
    [string[]]$RunnerLabel = @('self-hosted', 'Windows', 'X64', 'ephemeral', 'uci-suite'),
    [string]$RunnerSelection = 'ephemeral-self-hosted-windows',
    [string]$RunnerCacheRoot = 'C:\chess_engine\.actions-runner-cache',
    [string]$RunnerWorkRoot = 'C:\chess_engine\.release',
    [string]$LogRoot = '',
    [ValidateRange(5, 180)][int]$TimeoutMinutes = 90,
    [switch]$PreflightOnly,
    [switch]$AllowUnmergedDiagnostic
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runnerVersion = '2.336.0'
$runnerArchiveName = "actions-runner-win-x64-$runnerVersion.zip"
$runnerArchiveSha256 = 'd59123a43003e357b0805b5d0f611d0bd2f65ab67d51bd070dd4e7a0f685c162'
$runnerArchiveUrl = "https://github.com/actions/runner/releases/download/v$runnerVersion/$runnerArchiveName"
$maximumLogBytes = 64MB

function Assert-Command {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) { throw "Required release command is unavailable: $Name" }
    return $command.Source
}

function Invoke-GitHubJson {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )
    for ($attempt = 1; $attempt -le 3; $attempt += 1) {
        $output = & gh @Arguments 2>$null
        if ($LASTEXITCODE -eq 0 -and $output) {
            try { return $output | ConvertFrom-Json -ErrorAction Stop } catch {}
        }
        if ($attempt -lt 3) { Start-Sleep -Milliseconds 500 }
    }
    throw $FailureMessage
}

function Assert-GitHubAuthentication {
    for ($attempt = 1; $attempt -le 3; $attempt += 1) {
        & gh auth status --hostname github.com *> $null
        if ($LASTEXITCODE -eq 0) { return }
        if ($attempt -lt 3) { Start-Sleep -Milliseconds 500 }
    }
    throw 'GitHub CLI authentication is unavailable.'
}

function Import-VisualStudioEnvironment {
    $vswhereCandidates = @(@(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($vswhereCandidates.Count -lt 1) { throw 'vswhere.exe is unavailable.' }
    $installation = & $vswhereCandidates[0] -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        throw 'Visual Studio C++ Build Tools are unavailable.'
    }
    $vsDevCmd = Join-Path $installation.Trim() 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw 'VsDevCmd.bat is unavailable.'
    }
    $temporaryBase = [System.IO.Path]::GetTempFileName()
    $batch = "$temporaryBase.cmd"
    try {
        @(
            '@echo off',
            "call `"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul",
            'if errorlevel 1 exit /b %errorlevel%',
            'set'
        ) | Set-Content -LiteralPath $batch -Encoding Ascii
        $environmentLines = & cmd.exe /d /c $batch
        if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
        foreach ($line in $environmentLines) {
            $separator = $line.IndexOf('=')
            if ($separator -le 0) { continue }
            $name = $line.Substring(0, $separator)
            if ($name.StartsWith('=')) { continue }
            [Environment]::SetEnvironmentVariable(
                $name, $line.Substring($separator + 1), 'Process')
        }
    } finally {
        Remove-Item -LiteralPath $temporaryBase, $batch -Force -ErrorAction SilentlyContinue
    }
}

function Test-RunnerArchive {
    param([string]$Filename)
    if (-not (Test-Path -LiteralPath $Filename -PathType Leaf)) { return $false }
    return (Get-FileHash -LiteralPath $Filename -Algorithm SHA256).Hash.ToLowerInvariant() `
        -eq $runnerArchiveSha256
}

if ($env:OS -ne 'Windows_NT') { throw 'The local ephemeral release runner requires Windows.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'Repository must be an owner/name slug.'
}
if ($Ref -notmatch '^[A-Za-z0-9._/-]+$' -or $Ref.Contains('..')) {
    throw 'Ref is invalid.'
}
if ($Ref -ne 'main' -and -not $AllowUnmergedDiagnostic) {
    throw 'Normal component releases require Ref main at the exact integrated remote main head. Use -AllowUnmergedDiagnostic only for an unpublished diagnostic run.'
}
if ($Target -ne 'windows-x86_64') { throw 'The local ephemeral runner is Windows-only.' }
if ($RunnerLabel.Count -lt 5 -or $RunnerLabel.Count -ne @($RunnerLabel | Select-Object -Unique).Count) {
    throw 'Runner labels must be unique and include the release authority labels.'
}
foreach ($label in $RunnerLabel) {
    if ($label -notmatch '^[A-Za-z0-9_.-]+$') { throw "Runner label is invalid: $label" }
}

$repositoryRoot = (Resolve-Path -LiteralPath $RepositoryPath).Path
$workflowPath = Join-Path $repositoryRoot ".github\workflows\$Workflow"
if (-not (Test-Path -LiteralPath $workflowPath -PathType Leaf)) {
    throw "Release workflow is unavailable: $Workflow"
}
$trackedStatus = (& git -C $repositoryRoot status --porcelain --untracked-files=no)
if ($LASTEXITCODE -ne 0 -or $trackedStatus) {
    throw 'Component release requires a clean tracked worktree.'
}
$head = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
    throw 'Local source commit identity is unavailable.'
}

Import-VisualStudioEnvironment
$gitBin = Join-Path $env:ProgramFiles 'Git\bin'
$gitCmd = Join-Path $env:ProgramFiles 'Git\cmd'
if (Test-Path -LiteralPath $gitBin) { $env:PATH = "$gitBin;$env:PATH" }
if (Test-Path -LiteralPath $gitCmd) { $env:PATH = "$gitCmd;$env:PATH" }
$moduleRoots = @(
    'C:\Program Files\WindowsPowerShell\Modules',
    'C:\Windows\System32\WindowsPowerShell\v1.0\Modules'
) | Where-Object { Test-Path -LiteralPath $_ }
$env:PSModulePath = (@($moduleRoots) + @($env:PSModulePath)) -join ';'

foreach ($command in @(
    'cl.exe', 'msbuild.exe', 'cmake.exe', 'git.exe', 'gh.exe',
    'node.exe', 'bash.exe', 'powershell.exe'
)) {
    [void](Assert-Command $command)
}
if (Get-Command pwsh.exe -ErrorAction SilentlyContinue) {
    & pwsh.exe -NoProfile -NonInteractive -Command `
        'Get-Command Get-FileHash -ErrorAction Stop | Out-Null' *> $null
    if ($LASTEXITCODE -ne 0) { throw 'PowerShell 7 module resolution is invalid.' }
}
& powershell.exe -NoProfile -NonInteractive -Command `
    'Get-Command Get-FileHash -ErrorAction Stop | Out-Null' *> $null
if ($LASTEXITCODE -ne 0) { throw 'Windows PowerShell module resolution is invalid.' }
$bash = (Get-Command bash.exe).Source
& $bash -lc 'powershell.exe -NoProfile -NonInteractive -Command "Get-Command Get-FileHash -ErrorAction Stop | Out-Null"' *> $null
if ($LASTEXITCODE -ne 0) { throw 'Git Bash cannot resolve the required PowerShell modules.' }

Assert-GitHubAuthentication
$remoteHead = (& git -C $repositoryRoot ls-remote origin "refs/heads/$Ref").Trim()
if ($LASTEXITCODE -ne 0 -or -not $remoteHead) {
    throw "Remote release ref is unavailable: $Ref"
}
$remoteCommit = ($remoteHead -split '\s+')[0]
if ($remoteCommit -ne $head) {
    throw "Remote release ref is stale: local $head, remote $remoteCommit"
}
$remoteMain = (& git -C $repositoryRoot ls-remote origin 'refs/heads/main').Trim()
if ($LASTEXITCODE -ne 0 -or -not $remoteMain) {
    throw 'Remote main release authority is unavailable.'
}
$remoteMainCommit = ($remoteMain -split '\s+')[0]

$preflightScript = Join-Path $PSScriptRoot 'local-release-preflight.mjs'
$preflightArguments = @(
    $preflightScript,
    '--root', $repositoryRoot,
    '--version', $Version,
    '--component', $ComponentId,
    '--workflow', ".github/workflows/$Workflow",
    '--ref', $Ref,
    '--head', $head,
    '--remote-main-head', $remoteMainCommit,
    '--allow-unmerged-diagnostic', $AllowUnmergedDiagnostic.IsPresent.ToString().ToLowerInvariant(),
    '--runner-work-root', $RunnerWorkRoot,
    '--repository', $Repository
)
foreach ($label in $RunnerLabel) {
    $preflightArguments += @('--runner-label', $label)
}
$contractOutput = & node @preflightArguments
if ($LASTEXITCODE -ne 0) { throw 'Component release source contract preflight failed.' }
$contract = $contractOutput | ConvertFrom-Json
if ($contract.schema -ne 'arena_local_release_preflight_v1') {
    throw 'Component release source preflight returned an invalid result.'
}

$runnerArchive = Join-Path $RunnerCacheRoot $runnerArchiveName
if (-not (Test-RunnerArchive $runnerArchive)) {
    if ($PreflightOnly) {
        throw "Pinned Actions runner archive is unavailable or invalid: $runnerArchive"
    }
    New-Item -ItemType Directory -Path $RunnerCacheRoot -Force | Out-Null
    $download = "$runnerArchive.download-$PID"
    try {
        Invoke-WebRequest -Uri $runnerArchiveUrl -OutFile $download -UseBasicParsing
        if (-not (Test-RunnerArchive $download)) {
            throw 'Downloaded Actions runner archive failed its pinned SHA-256.'
        }
        Move-Item -LiteralPath $download -Destination $runnerArchive -Force
    } finally {
        Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue
    }
}

if ($PreflightOnly) {
    [pscustomobject]@{
        schema = 'arena_local_ephemeral_release_preflight_v1'
        state = 'ready'
        repository = $Repository
        ref = $Ref
        head = $head
        version = $Version
        workflow = $Workflow
        runner_version = $runnerVersion
        labels = $RunnerLabel.Count
        authority = $contract.authority
    } | ConvertTo-Json -Compress
    return
}

if (-not $LogRoot) { $LogRoot = Join-Path $PSScriptRoot '..\..\build\release-runner-logs' }
$resolvedLogRoot = [System.IO.Path]::GetFullPath($LogRoot)
New-Item -ItemType Directory -Path $resolvedLogRoot -Force | Out-Null
$runKey = "{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $head.Substring(0, 8)
$stdoutLog = Join-Path $resolvedLogRoot "$runKey.stdout.log"
$stderrLog = Join-Path $resolvedLogRoot "$runKey.stderr.log"
$runnerBase = $contract.runner_work_root
New-Item -ItemType Directory -Path $runnerBase -Force | Out-Null
$runnerRoot = Join-Path $runnerBase ([guid]::NewGuid().ToString('N'))
$runnerProcess = $null
$configured = $false
$runId = $null
$completed = $false
$cleanupAuthorized = $true
try {
    New-Item -ItemType Directory -Path $runnerRoot | Out-Null
    Expand-Archive -LiteralPath $runnerArchive -DestinationPath $runnerRoot
    $registrationToken = (& gh api --method POST `
        "repos/$Repository/actions/runners/registration-token" --jq .token).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $registrationToken) {
        throw 'Could not obtain a just-in-time runner registration token.'
    }
    try {
        $runnerName = "uci-suite-$env:COMPUTERNAME-$($head.Substring(0, 8))-$PID"
        & (Join-Path $runnerRoot 'config.cmd') --unattended --ephemeral --disableupdate `
            --url "https://github.com/$Repository" --token $registrationToken `
            --name $runnerName --labels ($RunnerLabel -join ',') --work '_work' *> $null
        if ($LASTEXITCODE -ne 0) { throw 'Ephemeral runner configuration failed.' }
        $configured = $true
    } finally {
        $registrationToken = $null
    }

    $runnerProcess = Start-Process -FilePath (Join-Path $runnerRoot 'run.cmd') `
        -WorkingDirectory $runnerRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
    $dispatchTime = [DateTimeOffset]::UtcNow
    $dispatchArguments = @(
        'workflow', 'run', $Workflow, '--repo', $Repository, '--ref', $Ref,
        '-f', "version=$Version", '-f', "target=$Target",
        '-f', "runner=$RunnerSelection"
    )
    if ($WorkflowComponent) {
        $dispatchArguments += @('-f', "component=$WorkflowComponent")
    }
    & gh @dispatchArguments *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Workflow dispatch failed.' }

    $deadline = [DateTimeOffset]::UtcNow.AddMinutes($TimeoutMinutes)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if ((Test-Path $stdoutLog) -and (Get-Item $stdoutLog).Length -gt $maximumLogBytes) {
            throw 'Runner stdout exceeded the bounded log limit.'
        }
        if ((Test-Path $stderrLog) -and (Get-Item $stderrLog).Length -gt $maximumLogBytes) {
            throw 'Runner stderr exceeded the bounded log limit.'
        }
        if (-not $runId) {
            $runs = @(Invoke-GitHubJson -Arguments @(
                'run', 'list', '--repo', $Repository, '--workflow', $Workflow,
                '--branch', $Ref, '--event', 'workflow_dispatch', '--limit', '10',
                '--json', 'databaseId,headSha,status,conclusion,createdAt'
            ) -FailureMessage 'Could not query the dispatched workflow after three attempts.')
            $matching = @($runs | Where-Object {
                $_.headSha -eq $head -and
                [DateTimeOffset]::Parse($_.createdAt) -ge $dispatchTime.AddSeconds(-5)
            } | Sort-Object { [DateTimeOffset]::Parse($_.createdAt) })
            if ($matching.Count -gt 1) {
                throw 'More than one matching workflow run exists; refusing ambiguous monitoring.'
            }
            if ($matching.Count -eq 1) { $runId = [string]$matching[0].databaseId }
        } else {
            $run = Invoke-GitHubJson -Arguments @(
                'run', 'view', $runId, '--repo', $Repository,
                '--json', 'databaseId,headSha,status,conclusion'
            ) -FailureMessage 'Could not inspect the dispatched workflow after three attempts.'
            if ($run.headSha -ne $head) { throw 'Dispatched workflow head changed unexpectedly.' }
            if ($run.status -eq 'completed') {
                $completed = $true
                if ($run.conclusion -ne 'success') {
                    throw "Component release concluded $($run.conclusion)."
                }
                break
            }
        }
        Start-Sleep -Seconds 10
    }
    if (-not $completed) { throw 'Component release exceeded its one-attempt deadline.' }

    [pscustomobject]@{
        schema = 'arena_local_ephemeral_release_result_v1'
        state = 'completed'
        repository = $Repository
        ref = $Ref
        head = $head
        workflow_run_id = $runId
        runner_version = $runnerVersion
        stdout_log = $stdoutLog
        stderr_log = $stderrLog
    } | ConvertTo-Json -Compress
} catch {
    $releaseFailure = $_
    if ($runId -and -not $completed) {
        & gh run cancel $runId --repo $Repository *> $null
        $drainDeadline = [DateTimeOffset]::UtcNow.AddMinutes(5)
        $runDrained = $false
        while ([DateTimeOffset]::UtcNow -lt $drainDeadline) {
            try {
                $drain = Invoke-GitHubJson -Arguments @(
                    'run', 'view', $runId, '--repo', $Repository,
                    '--json', 'status,conclusion'
                ) -FailureMessage 'Could not inspect cancellation state.'
                $runDrained = $drain.status -eq 'completed'
            } catch {
                $runDrained = $false
            }
            $runnerDrained = -not $runnerProcess -or $runnerProcess.HasExited
            if ($runDrained -and $runnerDrained) { break }
            Start-Sleep -Seconds 2
        }
        if (-not $runDrained -or ($runnerProcess -and -not $runnerProcess.HasExited)) {
            $cleanupAuthorized = $false
            Write-Error "Release cancellation did not drain; preserving ephemeral runner root for the active job: $runnerRoot"
        }
    }
    Write-Error "Local ephemeral release failed: $($releaseFailure.Exception.Message)"
    throw $releaseFailure
} finally {
    if ($cleanupAuthorized -and $runnerProcess -and -not $runnerProcess.HasExited) {
        Stop-Process -Id $runnerProcess.Id -Force -ErrorAction SilentlyContinue
        $runnerProcess.WaitForExit(5000)
    }
    if ($cleanupAuthorized -and $configured -and
        (Test-Path -LiteralPath (Join-Path $runnerRoot 'config.cmd'))) {
        $removeToken = (& gh api --method POST `
            "repos/$Repository/actions/runners/remove-token" --jq .token 2>$null)
        if ($LASTEXITCODE -eq 0 -and $removeToken) {
            & (Join-Path $runnerRoot 'config.cmd') remove --token $removeToken.Trim() *> $null
        }
        $removeToken = $null
    }
    $resolvedRunnerBase = [System.IO.Path]::GetFullPath($runnerBase).TrimEnd('\') + '\'
    $resolvedRunnerRoot = [System.IO.Path]::GetFullPath($runnerRoot)
    if ($cleanupAuthorized -and
        $resolvedRunnerRoot.StartsWith($resolvedRunnerBase, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedRunnerRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
