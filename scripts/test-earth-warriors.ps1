# Offline regression for the former monkey-bar crossing. Run after scripts/bake.ps1.
[CmdletBinding()]
param(
    [string] $Artifact = "$PSScriptRoot\..\build\worldwalker.wwa",
    [string] $Cli = "$PSScriptRoot\..\build\Release\wwcli.exe"
)
$ErrorActionPreference = 'Stop'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName() + '.json')
$cases = @(
    @{ Name='surface north at 15'; Start=@(3094,3492,0); Goal=@(3118,9974,0); Level=15; Loc=127964 },
    @{ Name='dungeon north at 15'; Start=@(3096,9867,0); Goal=@(3118,9974,0); Level=15; Loc=127964 },
    @{ Name='south at 15'; Start=@(3118,9974,0); Goal=@(3096,9867,0); Level=15; Loc=127966 },
    @{ Name='north below 15'; Start=@(3096,9867,0); Goal=@(3118,9974,0); Level=14; Loc=0 },
    @{ Name='south below 15'; Start=@(3118,9974,0); Goal=@(3096,9867,0); Level=14; Loc=0 },
    @{ Name='shortcuts disabled'; Start=@(3096,9867,0); Goal=@(3118,9974,0); Level=15; Loc=0; Disable='shortcuts' },
    @{ Name='free to play'; Start=@(3096,9867,0); Goal=@(3118,9974,0); Level=15; Loc=0; Disable='f2p' }
)
try {
    foreach ($case in $cases) {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
        $arguments = @('path', $Artifact) + $case.Start + $case.Goal + @('--skill', "16=$($case.Level)", '--out', $temporary)
        if ($case.Disable) { $arguments += @('--disable', $case.Disable) }
        & $Cli @arguments
        $code = $LASTEXITCODE
        if ($case.Loc -eq 0) {
            if ($code -ne 2) { throw "$($case.Name): expected no route, exit=$code" }
        } else {
            if ($code -ne 0) { throw "$($case.Name): expected route, exit=$code" }
            $path = Get-Content -Raw -LiteralPath $temporary | ConvertFrom-Json
            # Require the intended direction's loc, not merely any successful route.
            if (-not ($path.steps | Where-Object { $_.objectId -eq $case.Loc })) {
                throw "$($case.Name): missing crossing loc $($case.Loc)"
            }
        }
        Write-Host "PASS: $($case.Name)"
    }
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}

