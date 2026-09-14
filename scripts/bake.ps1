# Regenerates the shipping WorldWalker artifact from tracked inputs.
#
# ONE COMMAND, and it has to work for somebody who has never run this before:
#
#     .\scripts\bake.ps1
#
# THE CACHE IS NOT SOMETHING YOU HAVE TO BE GIVEN. <cache_dir> is the RuneScape 3
# NXT client's own cache directory -- by default C:\ProgramData\Jagex\RuneScape,
# a set of js5-<N>.jcache SQLite databases that every RS3 install already has.
# Nobody needs a multi-GB cache dump handed to them by the maintainer, which is
# the whole reason an outside contributor can rebake at all.
#
# --live IS THE DEFAULT, AND THAT IS DELIBERATE. A player's local cache holds only
# what that player has actually downloaded, so a local-only bake is
# contributor-dependent: two people bake the same datasets and get different
# artifacts, with no signal saying why. NXTCacheLibrary completes missing
# reference tables and archives from the live JS5 servers on demand (RSCache /
# Index fallback), so --live is what makes every contributor's bake converge on
# the same complete world. -NoLive exists for deliberately baking exactly what is
# on disk. A cold cache means tens of thousands of archive fetches and a long,
# network-bound run; a normal player's cache only needs the gaps filled.
#
# PROVENANCE IS THE POINT, not a side effect. wwbuild stamps the artifact with
# what it was baked from and drops the same record beside it as <out>.json. This
# script supplies the two things wwbuild cannot know -- the source and dataset
# revisions -- from git, and adds the artifact's own SHA256 to the sidecar after
# the fact. wwbuild deliberately does not shell out to git itself: a build tool
# guessing at its own version is how a confident wrong answer gets recorded.
#
# WHAT THIS SCRIPT DOES NOT DO: publish. It writes two files and prints the
# command that would publish them. Uploading is a deliberate act, not a side
# effect of baking -- see docs/artifact-delivery.md.
#
# Usage:
#   .\scripts\bake.ps1
#   .\scripts\bake.ps1 -CacheDir D:\somewhere\else -NoLive
#   .\scripts\bake.ps1 -Out build\scratch.wwa       # a bake you do not intend to ship
#
# Exit codes: 0 = baked and passed check, 1 = bake or check failed, 2 = inputs missing.

[CmdletBinding()]
param(
    # The RS3 NXT cache directory. Defaults to the client's own.
    [string] $CacheDir = "",

    # Where the artifact lands. The default IS the convention: build\worldwalker.wwa
    # is the only name the deploy payload reads, so the bake writes that name and
    # no other unless you say otherwise. Everything else under build\ is scratch.
    [string] $Out = "",

    # Tracked, version-controlled transition datasets. Rarely worth overriding.
    [string] $DatasetDir = "",

    # Bake only what is in the local cache; do not complete it from the live
    # servers. Produces a machine-dependent artifact -- see the note above.
    [switch] $NoLive,

    # Skip the post-bake acceptance check. Here for debugging a broken bake, not
    # for shipping one.
    [switch] $SkipCheck,

    # The release tag this bake is destined for (e.g. "wwa-2026-09-13"). Supplying
    # it stamps the tag into the sidecar and copies the sidecar to the TRACKED
    # manifest at artifacts\worldwalker.wwa.json, ready to commit.
    #
    # It does not upload anything. The manifest is a claim about where the
    # artifact will live, and claiming it is a deliberate act -- which is why it
    # takes a flag rather than happening on every scratch bake. The sidecar that
    # lands beside the artifact in build\ is gitignored like everything else
    # there; artifacts\ is the only copy git tracks.
    [string] $ReleaseTag = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $CacheDir)   { $CacheDir   = Join-Path $env:ProgramData "Jagex\RuneScape" }
if (-not $Out)        { $Out        = Join-Path $RepoRoot "build\worldwalker.wwa" }
if (-not $DatasetDir) { $DatasetDir = Join-Path $RepoRoot "datasets" }

$BuildDir = Join-Path $RepoRoot "build\Release"
$WwBuild  = Join-Path $BuildDir "wwbuild.exe"
$WwCli    = Join-Path $BuildDir "wwcli.exe"

function Write-Step { param([string] $Message) Write-Host "==> $Message" }

# Every documented exit code reaches the caller through here. $ErrorActionPreference
# = "Stop" makes Write-Error terminating, so a `Write-Error ...; exit 2` pair never
# reaches its own exit -- PowerShell unwinds first and the process leaves with 1,
# which makes every documented exit code a fiction. (deploy-data-zip.ps1 learned
# this the hard way; same fix.)
function Fail {
    param([string] $Message, [int] $Code)
    $host.UI.WriteErrorLine($Message)
    exit $Code
}

# git is not always on PATH in a stripped environment, and "git is missing" and
# "this is not a checkout" are different answers. Returns $null when git cannot be
# found or the command fails; the caller records nothing rather than guessing.
function Get-GitExe {
    $candidates = @(
        "git.exe",
        "C:\Program Files\Git\cmd\git.exe",
        "C:\Program Files (x86)\Git\cmd\git.exe"
    )
    foreach ($candidate in $candidates) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) { return $found.Source }
    }
    return $null
}

function Invoke-Git {
    param([string] $GitExe, [string[]] $GitArgs)
    if (-not $GitExe) { return $null }
    try {
        $output = & $GitExe -C $RepoRoot @GitArgs
        if ($LASTEXITCODE -ne 0) { return $null }
        return ($output | Out-String).Trim()
    } catch {
        return $null
    }
}

# ---- Inputs ---------------------------------------------------------------

Write-Step "Checking inputs"

if (-not (Test-Path $WwBuild)) {
    Fail @"
wwbuild.exe not found at $WwBuild

Build it first:
  cmake -B build
  cmake --build build --config Release

(Run both from a vcvars64 shell. WorldWalker links NXTCacheLibrary's prebuilt
NXTCache.dll from ..\NXTCacheLibrary\build\<CONFIG>\ -- if that sibling repo is
not built, the configure step is where you will find out.)
"@ 2
}

# NXTCache.dll is loaded at run time, not link time: without it beside wwbuild.exe
# the bake dies with a Windows loader error that says nothing about the cause.
if (-not (Test-Path (Join-Path $BuildDir "NXTCache.dll"))) {
    Fail "NXTCache.dll not found beside wwbuild.exe in $BuildDir -- build NXTCacheLibrary, then re-run cmake --build here." 2
}

if (-not (Test-Path $CacheDir)) {
    Fail @"
Cache directory not found: $CacheDir

This should be the RuneScape 3 NXT client's cache -- the directory holding
js5-0.jcache, js5-1.jcache, ... Install/run RS3 once and it will be there, or
pass -CacheDir explicitly.
"@ 2
}

$jcacheCount = @(Get-ChildItem -Path $CacheDir -Filter "js5-*.jcache" -ErrorAction SilentlyContinue).Count
if ($jcacheCount -eq 0 -and $NoLive) {
    Fail "No js5-*.jcache files in $CacheDir and -NoLive was given, so there is nothing to bake from." 2
}

if (-not (Test-Path $DatasetDir)) {
    Fail "Dataset directory not found: $DatasetDir" 2
}

$OutDir = Split-Path -Parent $Out
if ($OutDir -and -not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

# ---- Provenance inputs git can answer and wwbuild cannot ------------------

$GitExe = Get-GitExe
$sourceVersion  = Invoke-Git $GitExe @("describe", "--always", "--dirty", "--tags")
$datasetVersion = Invoke-Git $GitExe @("log", "-1", "--format=%h", "--", "datasets")
if ($datasetVersion) {
    $datasetDirty = Invoke-Git $GitExe @("status", "--porcelain", "--", "datasets")
    if ($datasetDirty) { $datasetVersion = "$datasetVersion-dirty" }
}
if (-not $sourceVersion)  { Write-Host "    (source version unavailable -- recording null)" }
if (-not $datasetVersion) { Write-Host "    (dataset version unavailable -- recording null)" }

# ---- Bake ------------------------------------------------------------------

$isLive = -not $NoLive
$bakeArgs = @("build", $CacheDir, $DatasetDir, $Out)
if ($isLive) { $bakeArgs += "--live" }
if ($sourceVersion)  { $bakeArgs += @("--source-version", $sourceVersion) }
if ($datasetVersion) { $bakeArgs += @("--dataset-version", $datasetVersion) }

Write-Step "Baking $Out"
Write-Host "    cache:    $CacheDir ($jcacheCount js5-*.jcache present)"
if ($isLive) {
    Write-Host "    mode:     local + live fallback (missing data is fetched from the JS5 servers)"
    if ($jcacheCount -eq 0) {
        Write-Host "    WARNING:  the local cache is empty, so EVERYTHING comes over the network."
        Write-Host "              Expect a long, network-bound run."
    }
} else {
    Write-Host "    mode:     local only (-NoLive) -- this bakes exactly what is on disk,"
    Write-Host "              so the result depends on what this machine has downloaded."
}
Write-Host "    source:   $(if ($sourceVersion) { $sourceVersion } else { '(unrecorded)' })"
Write-Host "    datasets: $DatasetDir @ $(if ($datasetVersion) { $datasetVersion } else { '(unrecorded)' })"

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
& $WwBuild @bakeArgs
$bakeExit = $LASTEXITCODE
$stopwatch.Stop()

if ($bakeExit -ne 0) {
    Fail "wwbuild failed (exit $bakeExit) after $([math]::Round($stopwatch.Elapsed.TotalMinutes, 1)) min" 1
}
Write-Step "Baked in $([math]::Round($stopwatch.Elapsed.TotalMinutes, 2)) min"

# ---- Finish the sidecar ---------------------------------------------------

# wwbuild cannot hash its own output, and the artifact's SHA256 is the identity a
# release asset gets pinned by. Adding it here keeps the sidecar the single text
# file that fully describes the binary next to it.
# WriteAllText resolves relative paths against the process working directory, not
# PowerShell's, so make it absolute before handing it over.
$Out = (Resolve-Path -Path $Out).Path
$Sidecar = "$Out.json"
if (Test-Path $Sidecar) {
    $artifactHash = (Get-FileHash -Path $Out -Algorithm SHA256).Hash.ToLowerInvariant()
    $artifactBytes = (Get-Item $Out).Length

    $record = Get-Content -Raw -Path $Sidecar | ConvertFrom-Json
    $record | Add-Member -NotePropertyName "artifactSha256" -NotePropertyValue $artifactHash -Force
    $record | Add-Member -NotePropertyName "artifactBytes"  -NotePropertyValue $artifactBytes -Force
    if ($ReleaseTag) {
        $record | Add-Member -NotePropertyName "releaseTag" -NotePropertyValue $ReleaseTag -Force
    }
    # NOT Set-Content -Encoding utf8: PowerShell 5.1 spells that with a BOM, and a
    # BOM makes the file fail strict JSON parsers (python's json.load, jq) for a
    # manifest whose entire job is to be read by other tools.
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Sidecar, ($record | ConvertTo-Json -Depth 10), $utf8NoBom)

    Write-Step "Sidecar $Sidecar"
    Write-Host "    sha256: $artifactHash"
    Write-Host "    bytes:  $artifactBytes"

    if ($ReleaseTag) {
        $manifestDir = Join-Path $RepoRoot "artifacts"
        if (-not (Test-Path $manifestDir)) {
            New-Item -ItemType Directory -Force -Path $manifestDir | Out-Null
        }
        $manifest = Join-Path $manifestDir "worldwalker.wwa.json"
        Copy-Item -Path $Sidecar -Destination $manifest -Force
        Write-Step "Tracked manifest $manifest (tag $ReleaseTag) -- commit this"
    }
}

# ---- Acceptance gate -------------------------------------------------------

if (-not $SkipCheck) {
    Write-Step "Checking the bake"
    & $WwCli check $Out $DatasetDir
    if ($LASTEXITCODE -ne 0) {
        Fail "wwcli check failed -- do not ship this artifact." 1
    }
}

Write-Step "Done"
Write-Host ""
Write-Host "Publishing is a separate, deliberate act. To make this bake the record:"
Write-Host "  .\scripts\bake.ps1 -ReleaseTag wwa-$(Get-Date -Format 'yyyy-MM-dd')   # stamps + copies the manifest"
Write-Host "  gh release create wwa-$(Get-Date -Format 'yyyy-MM-dd') `"$Out`" -R BotWithUs/WorldWalker"
Write-Host "  git add artifacts\worldwalker.wwa.json   # the manifest is tracked; the 10 MB artifact is not"
Write-Host "See docs/artifact-delivery.md for why, and for what the deploy side expects."
exit 0
