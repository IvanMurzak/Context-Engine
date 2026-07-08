#Requires -Version 5.1
<#
.SYNOPSIS
    Register a GitHub Actions self-hosted runner as a Windows service, hardened for
    MULTIPLE runners on one machine (the Context Engine CI fleet).

.DESCRIPTION
    Idempotent, unattended setup that avoids every issue we hit standing up the
    context-engine Windows runner fleet (see docs/self-hosted-runners.md):

      * Multiple runners on ONE box share the service account's HOME, hence a single
        global git config. Concurrent jobs then race on `.gitconfig.lock`
        ("could not lock config file ...: File exists"), flaking the Windows legs.
        FIX: this script points each runner at its OWN `GIT_CONFIG_GLOBAL` (a per-runner
        `.gitconfig` pre-seeded with `safe.directory = *`) via the runner's `.env` file,
        so no two runners ever touch the same git-config file. This is the machine-side
        complement to the workflow-side env-injection in `.github/workflows/ci.yml`.

      * LocalSystem cannot run WSL, so workflow steps must use `pwsh`/`cmd`, never
        `shell: bash`. And Python is exposed as `py`, not `python`. (Documented; not
        enforced here — this script only provisions the runner.)

    Run it once per runner, giving each a distinct -RunnerName (and thus a distinct
    -RunnerRoot). Re-running with the same name re-registers cleanly (--replace).

.PARAMETER RepoUrl
    The repository (or org) URL to attach the runner to,
    e.g. https://github.com/IvanMurzak/Context-Engine

.PARAMETER Token
    A *registration* token (NOT a PAT). Get a fresh one (valid ~1h) from:
    Settings > Actions > Runners > New self-hosted runner, or:
      gh api -X POST repos/<owner>/<repo>/actions/runners/registration-token --jq .token

.PARAMETER RunnerName
    Unique runner name. Default: "<hostname>-<n>" is NOT auto-derived; pass it explicitly
    per runner (e.g. context-engine-win, context-engine-win-2, context-engine-win-3).

.PARAMETER Labels
    Comma-separated labels. The Context Engine CI targets the "context-engine" label
    (plus the implicit self-hosted/Windows/X64). Default: "context-engine".

.PARAMETER RunnerRoot
    Install directory. Default: C:\actions-runner-<RunnerName>. MUST be unique per runner.

.PARAMETER RunnerVersion
    actions/runner version, e.g. "2.319.1". Default: "latest" (resolved from the GitHub API).

.PARAMETER ExpectedSha256
    Optional SHA-256 of the runner zip for verify-before-use. If omitted the script prints
    the computed hash so you can pin it on the next run.

.PARAMETER ServiceAccount
    Windows account the service runs as. Default: "NT AUTHORITY\SYSTEM" (LocalSystem).
    RECOMMENDED for a hardened setup: a DEDICATED local user (its own profile => its own
    HOME/.gitconfig => the git race cannot happen even without the .env fix, and WSL works).
    Pass the account + -ServicePassword to use one.

.PARAMETER ServicePassword
    Password for -ServiceAccount when it is a real user account (omit for LocalSystem).

.EXAMPLE
    # LocalSystem (simplest); per-runner git isolation still applied.
    .\setup-self-hosted-runner.ps1 -RepoUrl https://github.com/IvanMurzak/Context-Engine `
        -Token <REG_TOKEN> -RunnerName context-engine-win-2

.EXAMPLE
    # Hardened: dedicated local user (own profile).
    .\setup-self-hosted-runner.ps1 -RepoUrl https://github.com/IvanMurzak/Context-Engine `
        -Token <REG_TOKEN> -RunnerName context-engine-win-3 `
        -ServiceAccount ".\gh-runner" -ServicePassword (Read-Host -AsSecureString | ConvertFrom-SecureString -AsPlainText)
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $RepoUrl,
    [Parameter(Mandatory = $true)] [string] $Token,
    [Parameter(Mandatory = $true)] [string] $RunnerName,
    [string] $Labels          = "context-engine",
    [string] $RunnerRoot,
    [string] $RunnerVersion   = "latest",
    [string] $ExpectedSha256,
    [string] $ServiceAccount  = "NT AUTHORITY\SYSTEM",
    [string] $ServicePassword
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Write-Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok($m)   { Write-Host "  [ok] $m"   -ForegroundColor Green }
function Write-Warn2($m){ Write-Host "  [warn] $m" -ForegroundColor Yellow }

# --- 0. Preconditions -------------------------------------------------------
Write-Step "Preconditions"
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
          ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "This script installs a Windows service and MUST run from an ELEVATED PowerShell (Run as Administrator)."
}
if ([Environment]::Is64BitOperatingSystem -eq $false) { throw "Windows x64 is required." }
if (-not $RunnerRoot) { $RunnerRoot = "C:\actions-runner-$RunnerName" }
Write-Ok "Elevated. Runner '$RunnerName' -> $RunnerRoot ; labels '$Labels'"

# --- 1. Resolve + download the runner package -------------------------------
Write-Step "Runner package"
if ($RunnerVersion -eq "latest") {
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/actions/runner/releases/latest" `
           -Headers @{ "User-Agent" = "context-engine-runner-setup" }
    $RunnerVersion = $rel.tag_name.TrimStart("v")
    Write-Ok "Resolved latest actions/runner = $RunnerVersion"
}
$zipName = "actions-runner-win-x64-$RunnerVersion.zip"
$zipUrl  = "https://github.com/actions/runner/releases/download/v$RunnerVersion/$zipName"

New-Item -ItemType Directory -Force -Path $RunnerRoot | Out-Null
$zipPath = Join-Path $RunnerRoot $zipName
if (Test-Path (Join-Path $RunnerRoot "config.cmd")) {
    Write-Warn2 "config.cmd already present in $RunnerRoot — reusing existing extraction (skip download)."
} else {
    Write-Ok "Downloading $zipUrl"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing
    $sha = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToLower()
    if ($ExpectedSha256) {
        if ($sha -ne $ExpectedSha256.ToLower()) { throw "SHA-256 mismatch: got $sha expected $ExpectedSha256 (refusing to use)." }
        Write-Ok "SHA-256 verified: $sha"
    } else {
        Write-Warn2 "No -ExpectedSha256 given. Computed SHA-256 = $sha  (pin it on the next run for verify-before-use)."
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $RunnerRoot)
    Write-Ok "Extracted to $RunnerRoot"
}

# --- 2. Per-runner git isolation (the .gitconfig-race fix) -------------------
# Give THIS runner its own global git config so concurrent runners never share a
# .gitconfig (and hence never race on .gitconfig.lock). Pre-seed safe.directory=*.
Write-Step "Per-runner git isolation"
$runnerGitConfig = Join-Path $RunnerRoot ".gitconfig"
@"
[safe]
	directory = *
"@ | Set-Content -Path $runnerGitConfig -Encoding ascii
# The runner applies variables from a `.env` file in its root to every job it runs.
$envFile = Join-Path $RunnerRoot ".env"
$envLines = @(
    "GIT_CONFIG_GLOBAL=$runnerGitConfig",
    "HOME=$RunnerRoot"
)
Set-Content -Path $envFile -Value $envLines -Encoding ascii
Write-Ok "Wrote $envFile  (GIT_CONFIG_GLOBAL -> $runnerGitConfig, safe.directory=*)"

# --- 3. Configure + install the service (unattended) ------------------------
Write-Step "Configure runner + install service"
Push-Location $RunnerRoot
try {
    $cfg = @(
        "--unattended", "--replace",
        "--url",    $RepoUrl,
        "--token",  $Token,
        "--name",   $RunnerName,
        "--labels", $Labels,
        "--work",   "_work",
        "--runasservice",
        "--windowslogonaccount", $ServiceAccount
    )
    if ($ServicePassword) { $cfg += @("--windowslogonpassword", $ServicePassword) }

    Write-Ok "Running config.cmd (unattended, runasservice, account='$ServiceAccount')"
    & "$RunnerRoot\config.cmd" @cfg
    if ($LASTEXITCODE -ne 0) { throw "config.cmd failed with exit code $LASTEXITCODE." }
}
finally { Pop-Location }

# --- 4. Ensure the service is running --------------------------------------
Write-Step "Service status"
# The runner service is named: actions.runner.<owner>-<repo>.<RunnerName>
$svc = Get-Service | Where-Object { $_.Name -like "actions.runner.*$RunnerName" } | Select-Object -First 1
if (-not $svc) { throw "Runner service not found after config — check the config.cmd output above." }
Set-Service -Name $svc.Name -StartupType Automatic
if ($svc.Status -ne "Running") { Start-Service -Name $svc.Name }
$svc = Get-Service -Name $svc.Name
Write-Ok "Service '$($svc.Name)' = $($svc.Status), StartType=Automatic"

# --- 5. Summary + reminders -------------------------------------------------
Write-Step "Done"
Write-Host @"
  Runner '$RunnerName' is registered and running as a service.

  Reminders for the WORKFLOW side (see docs/self-hosted-runners.md):
    * Use  shell: pwsh  (or cmd) on self-hosted Windows steps — NEVER  shell: bash
      (bash resolves to WSL, which cannot run under LocalSystem).
    * Call Python as  py , not  python .
    * safe.directory is handled two ways now (belt + suspenders): this runner's own
      GIT_CONFIG_GLOBAL (.env, set above) AND the workflow-level GIT_CONFIG_* env in ci.yml.
    * For a SECOND/THIRD runner on this machine, re-run with a DIFFERENT -RunnerName.

  Verify online: gh api repos/<owner>/<repo>/actions/runners --jq '.runners[].name'
"@ -ForegroundColor Green
