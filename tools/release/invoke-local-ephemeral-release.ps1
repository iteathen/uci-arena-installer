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
    [ValidateRange(1, 4)][int]$RunnerCount = 1,
    [string]$RunnerSelection = 'ephemeral-self-hosted-windows',
    [string]$CandidateReleaseTag = '',
    [string]$OnnxRuntimeUrl = '',
    [string]$OnnxRuntimeSha256 = '',
    [string]$ModelUrl = '',
    [string]$ModelSha256 = '',
    [string]$SyzygyManifestUrl = '',
    [string]$SyzygyManifestSha256 = '',
    [string]$RunnerCacheRoot = 'C:\chess_engine\.actions-runner-cache',
    [string]$RunnerWorkRoot = 'C:\ar',
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

function Assert-PinnedReleaseInputPair {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Url,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Sha256
    )
    if ([string]::IsNullOrEmpty($Url) -ne [string]::IsNullOrEmpty($Sha256)) {
        throw "$Label requires both URL and SHA-256."
    }
    if ([string]::IsNullOrEmpty($Url)) { return $false }
    if ($Url -match '[\x00-\x20\x7f]' -or $Sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "$Label requires a credential-free HTTPS URL and lowercase SHA-256."
    }
    try { $parsedUrl = [System.Uri]::new($Url, [System.UriKind]::Absolute) } catch {
        throw "$Label URL is invalid."
    }
    if ($parsedUrl.Scheme -ne 'https' -or -not $parsedUrl.Host -or $parsedUrl.UserInfo -or
        $parsedUrl.Query -or $parsedUrl.Fragment) {
        throw "$Label requires a credential-free HTTPS URL and lowercase SHA-256."
    }
    return $true
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

function Get-CandidateRelease {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Tag
    )
    for ($attempt = 1; $attempt -le 3; $attempt += 1) {
        $output = & gh release view $Tag --repo $Repository `
            --json assets,isDraft,isPrerelease,targetCommitish,tagName 2>$null
        if ($LASTEXITCODE -eq 0 -and $output) {
            try { return $output | ConvertFrom-Json -ErrorAction Stop } catch {}
        }
        if ($attempt -lt 3) { Start-Sleep -Milliseconds 500 }
    }
    return $null
}

function Ensure-EmptyCandidateRelease {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string]$Head,
        [Parameter(Mandatory = $true)][string]$ComponentId,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Target
    )
    $release = Get-CandidateRelease -Repository $Repository -Tag $Tag
    $state = 'verified'
    if (-not $release) {
        try {
            $created = Invoke-GitHubJson -Arguments @(
                'api', '--method', 'POST', "repos/$Repository/releases",
                '-f', "tag_name=$Tag", '-f', "target_commitish=$Head",
                '-f', "name=$ComponentId $Version $Target",
                '-f', 'body=Private immutable component candidate; verified workflow assets only.',
                '-F', 'draft=false', '-F', 'prerelease=true', '-f', 'make_latest=false'
            ) -FailureMessage 'Could not create the empty exact-head candidate prerelease.'
            $release = [pscustomobject]@{
                tagName = $created.tag_name
                isDraft = $created.draft
                isPrerelease = $created.prerelease
                targetCommitish = $created.target_commitish
                assets = @($created.assets)
            }
            $state = 'created'
        } catch {
            $release = Get-CandidateRelease -Repository $Repository -Tag $Tag
        }
        if (-not $release) { throw 'Candidate prerelease could not be created or verified.' }
    }
    if ($release.tagName -ne $Tag) { throw 'Candidate prerelease tag changed unexpectedly.' }
    if ($release.isDraft) { throw 'Candidate prerelease must not be a draft.' }
    if (-not $release.isPrerelease) { throw 'Candidate release must remain a prerelease.' }
    if ($release.targetCommitish -ne $Head) { throw 'Candidate prerelease targets a different source head.' }
    if (@($release.assets).Count -ne 0) { throw 'Candidate prerelease already contains assets.' }
    return $state
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
$releaseInputPairs = @(
    [pscustomobject]@{
        label = 'ONNX Runtime'; url_name = 'onnxruntime_url'; sha_name = 'onnxruntime_sha256'
        url = $OnnxRuntimeUrl; sha256 = $OnnxRuntimeSha256
    },
    [pscustomobject]@{
        label = 'Model'; url_name = 'model_url'; sha_name = 'model_sha256'
        url = $ModelUrl; sha256 = $ModelSha256
    },
    [pscustomobject]@{
        label = 'Syzygy manifest'; url_name = 'syzygy_manifest_url'
        sha_name = 'syzygy_manifest_sha256'; url = $SyzygyManifestUrl
        sha256 = $SyzygyManifestSha256
    }
)
$workflowSource = Get-Content -LiteralPath $workflowPath -Raw
$validatedReleaseInputPairs = @()
foreach ($pair in $releaseInputPairs) {
    if (-not (Assert-PinnedReleaseInputPair `
            -Label $pair.label -Url $pair.url -Sha256 $pair.sha256)) { continue }
    if ($workflowSource -notmatch "(?m)^\s+$([regex]::Escape($pair.url_name)):\s*$" -or
        $workflowSource -notmatch "(?m)^\s+$([regex]::Escape($pair.sha_name)):\s*$") {
        throw "$($pair.label) is not declared by the selected release workflow."
    }
    $validatedReleaseInputPairs += $pair
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
    '--repository', $Repository,
    '--candidate-release-tag', $CandidateReleaseTag
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
        runner_count = $RunnerCount
        pinned_input_pairs = $validatedReleaseInputPairs.Count
        authority = $contract.authority
        runner_work_root = $contract.runner_work_root
        projected_path_characters = $contract.projected_path_characters
        candidate_release_tag = $contract.candidate_release_tag
        candidate_release_action = $(if ($contract.candidate_release_tag) { 'would_create_or_verify' } else { 'not_requested' })
    } | ConvertTo-Json -Compress
    return
}

$candidateReleaseState = 'not_requested'
if ($contract.candidate_release_tag) {
    $candidateReleaseState = Ensure-EmptyCandidateRelease `
        -Repository $Repository -Tag $contract.candidate_release_tag -Head $head `
        -ComponentId $ComponentId -Version $Version -Target $Target
}

if (-not $LogRoot) { $LogRoot = Join-Path $PSScriptRoot '..\..\build\release-runner-logs' }
$resolvedLogRoot = [System.IO.Path]::GetFullPath($LogRoot)
New-Item -ItemType Directory -Path $resolvedLogRoot -Force | Out-Null
$runKey = "{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $head.Substring(0, 8)
$runnerBase = $contract.runner_work_root
New-Item -ItemType Directory -Path $runnerBase -Force | Out-Null
$runnerRoots = @()
$runnerProcesses = @()
$stdoutLogs = @()
$stderrLogs = @()
$runId = $null
$completed = $false
$cleanupAuthorized = $true
try {
    foreach ($runnerIndex in 1..$RunnerCount) {
        $runnerRoot = Join-Path $runnerBase ([guid]::NewGuid().ToString('N').Substring(0, 16))
        $stdoutLog = Join-Path $resolvedLogRoot "$runKey.runner-$runnerIndex.stdout.log"
        $stderrLog = Join-Path $resolvedLogRoot "$runKey.runner-$runnerIndex.stderr.log"
        $runnerRoots += $runnerRoot
        $stdoutLogs += $stdoutLog
        $stderrLogs += $stderrLog
        New-Item -ItemType Directory -Path $runnerRoot | Out-Null
        Expand-Archive -LiteralPath $runnerArchive -DestinationPath $runnerRoot
        $registrationToken = (& gh api --method POST `
            "repos/$Repository/actions/runners/registration-token" --jq .token).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $registrationToken) {
            throw 'Could not obtain a just-in-time runner registration token.'
        }
        try {
            $runnerName = "uci-suite-$env:COMPUTERNAME-$($head.Substring(0, 8))-$PID-$runnerIndex"
            & (Join-Path $runnerRoot 'config.cmd') --unattended --ephemeral --disableupdate `
                --url "https://github.com/$Repository" --token $registrationToken `
                --name $runnerName --labels ($RunnerLabel -join ',') --work 'w' *> $null
            if ($LASTEXITCODE -ne 0) { throw 'Ephemeral runner configuration failed.' }
        } finally {
            $registrationToken = $null
        }

        $runnerProcess = Start-Process -FilePath (Join-Path $runnerRoot 'run.cmd') `
            -WorkingDirectory $runnerRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
        $runnerProcesses += $runnerProcess
    }
    $priorRuns = @(Invoke-GitHubJson -Arguments @(
        'run', 'list', '--repo', $Repository, '--workflow', $Workflow,
        '--branch', $Ref, '--event', 'workflow_dispatch', '--limit', '10',
        '--json', 'databaseId,headSha,status,conclusion,createdAt'
    ) -FailureMessage 'Could not snapshot workflow runs before dispatch.')
    $priorRunIds = @($priorRuns | ForEach-Object { [string]$_.databaseId })
    $dispatchArguments = @(
        'workflow', 'run', $Workflow, '--repo', $Repository, '--ref', $Ref,
        '-f', "version=$Version", '-f', "target=$Target",
        '-f', "runner=$RunnerSelection"
    )
    if ($WorkflowComponent) {
        $dispatchArguments += @('-f', "component=$WorkflowComponent")
    }
    foreach ($pair in $validatedReleaseInputPairs) {
        $dispatchArguments += @(
            '-f', "$($pair.url_name)=$($pair.url)",
            '-f', "$($pair.sha_name)=$($pair.sha256)"
        )
    }
    & gh @dispatchArguments *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Workflow dispatch failed.' }

    $deadline = [DateTimeOffset]::UtcNow.AddMinutes($TimeoutMinutes)
    $lastMonitorFailure = $null
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        foreach ($logPath in @($stdoutLogs) + @($stderrLogs)) {
            if ((Test-Path $logPath) -and (Get-Item $logPath).Length -gt $maximumLogBytes) {
                throw 'Runner output exceeded the bounded per-runner log limit.'
            }
        }
        if (-not $runId) {
            $runs = $null
            try {
                $runs = @(Invoke-GitHubJson -Arguments @(
                    'run', 'list', '--repo', $Repository, '--workflow', $Workflow,
                    '--branch', $Ref, '--event', 'workflow_dispatch', '--limit', '10',
                    '--json', 'databaseId,headSha,status,conclusion,createdAt'
                ) -FailureMessage 'Could not query the dispatched workflow after three attempts.')
                $lastMonitorFailure = $null
            } catch {
                $lastMonitorFailure = $_.Exception.Message
            }
            if ($runs) {
                $matching = @($runs | Where-Object {
                    $_.headSha -eq $head -and
                    -not ($priorRunIds -contains [string]$_.databaseId)
                } | Sort-Object { [int64]$_.databaseId })
                if ($matching.Count -gt 1) {
                    $cleanupAuthorized = $false
                    throw 'More than one new matching workflow run exists; preserving the runner and refusing ambiguous monitoring.'
                }
                if ($matching.Count -eq 1) { $runId = [string]$matching[0].databaseId }
            }
        } else {
            $run = $null
            try {
                $run = Invoke-GitHubJson -Arguments @(
                    'run', 'view', $runId, '--repo', $Repository,
                    '--json', 'databaseId,headSha,status,conclusion'
                ) -FailureMessage 'Could not inspect the dispatched workflow after three attempts.'
                $lastMonitorFailure = $null
            } catch {
                $lastMonitorFailure = $_.Exception.Message
            }
            if ($run) {
                if ($run.headSha -ne $head) { throw 'Dispatched workflow head changed unexpectedly.' }
                if ($run.status -eq 'completed') {
                    $completed = $true
                    if ($run.conclusion -ne 'success') {
                        throw "Component release concluded $($run.conclusion)."
                    }
                    break
                }
            }
        }
        Start-Sleep -Seconds 10
    }
    if (-not $completed) {
        $monitorDetail = if ($lastMonitorFailure) { " Last monitor error: $lastMonitorFailure" } else { '' }
        throw "Component release exceeded its one-attempt deadline.$monitorDetail"
    }

    [pscustomobject]@{
        schema = 'arena_local_ephemeral_release_result_v1'
        state = 'completed'
        repository = $Repository
        ref = $Ref
        head = $head
        workflow_run_id = $runId
        runner_version = $runnerVersion
        runner_count = $RunnerCount
        pinned_input_pairs = $validatedReleaseInputPairs.Count
        candidate_release_tag = $contract.candidate_release_tag
        candidate_release_state = $candidateReleaseState
        stdout_logs = $stdoutLogs
        stderr_logs = $stderrLogs
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
            $runnerDrained = @($runnerProcesses | Where-Object { -not $_.HasExited }).Count -eq 0
            if ($runDrained -and $runnerDrained) { break }
            Start-Sleep -Seconds 2
        }
        if (-not $runDrained -or @($runnerProcesses | Where-Object { -not $_.HasExited }).Count -gt 0) {
            $cleanupAuthorized = $false
            Write-Error "Release cancellation did not drain; preserving ephemeral runner roots for the active workflow: $($runnerRoots -join ', ')"
        }
    }
    Write-Error "Local ephemeral release failed: $($releaseFailure.Exception.Message)"
    throw $releaseFailure
} finally {
    if ($cleanupAuthorized) {
        foreach ($runnerProcess in $runnerProcesses) {
            if (-not $runnerProcess.HasExited) {
                Stop-Process -Id $runnerProcess.Id -Force -ErrorAction SilentlyContinue
                [void]$runnerProcess.WaitForExit(5000)
            }
        }
    }
    if ($cleanupAuthorized) {
        foreach ($runnerRoot in $runnerRoots) {
            if (-not (Test-Path -LiteralPath (Join-Path $runnerRoot 'config.cmd'))) {
                continue
            }
            $removeToken = (& gh api --method POST `
                "repos/$Repository/actions/runners/remove-token" --jq .token 2>$null)
            if ($LASTEXITCODE -eq 0 -and $removeToken) {
                & (Join-Path $runnerRoot 'config.cmd') remove `
                    --token $removeToken.Trim() *> $null
            }
            $removeToken = $null
        }
    }
    $resolvedRunnerBase = [System.IO.Path]::GetFullPath($runnerBase).TrimEnd('\') + '\'
    if ($cleanupAuthorized) {
        foreach ($runnerRoot in $runnerRoots) {
            $resolvedRunnerRoot = [System.IO.Path]::GetFullPath($runnerRoot)
            if ($resolvedRunnerRoot.StartsWith(
                    $resolvedRunnerBase,
                    [System.StringComparison]::OrdinalIgnoreCase
                )) {
                Remove-Item -LiteralPath $resolvedRunnerRoot -Recurse -Force `
                    -ErrorAction SilentlyContinue
            }
        }
    }
}
