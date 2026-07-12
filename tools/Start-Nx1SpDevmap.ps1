param(
    [string]$Map = "nx_hospital",
    [string]$ExePath = (Join-Path $PSScriptRoot "..\nx1_sp\out\build\win-amd64-release\nx1_sp.exe"),
    [string]$GameDataRoot = (Join-Path $PSScriptRoot "..\nx1"),
    [string]$CachePath = "",
    [string]$ReadbackResolve = "",
    [switch]$NoProtectZero,
    [switch]$NoOcclusionQuery,
    [int]$FakeOcclusionSamples = 512,
    [switch]$UseGammaUnorm16,
    [string[]]$ExtraRuntimeArgs = @(),
    [string]$CommandPrefix = "+set logfile 1"
)

$ErrorActionPreference = "Stop"

function Quote-ProcessArgument {
    param([string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    return '"' + ($Value -replace '\\(?=")', '$0' -replace '"', '\"') + '"'
}

$resolvedExe = (Resolve-Path -LiteralPath $ExePath).Path
$resolvedGameData = (Resolve-Path -LiteralPath $GameDataRoot).Path
$workingDirectory = $resolvedGameData
$guestCommandLine = "$CommandPrefix +devmap $Map"

$runtimeArgs = @(
    "--allow_game_relative_writes",
    "--cl", $guestCommandLine
)

if ($CachePath -ne "") {
    $runtimeArgs += @("--cache_path", $CachePath)
}

if ($ReadbackResolve -ne "") {
    $runtimeArgs += @("--readback_resolve", $ReadbackResolve)
}

if ($NoProtectZero) {
    $runtimeArgs += "--no-protect_zero"
}

if ($NoOcclusionQuery) {
    $runtimeArgs += @("--no-occlusion_query_enable", "--query_occlusion_fake_sample_count", "$FakeOcclusionSamples")
}

if ($UseGammaUnorm16) {
    $runtimeArgs += "--nx1_gamma_render_target_as_unorm16"
} else {
    $runtimeArgs += "--no-gamma_render_target_as_unorm16"
}

$runtimeArgs += $ExtraRuntimeArgs

$argumentLine = ($runtimeArgs | ForEach-Object { Quote-ProcessArgument $_ }) -join " "

$processInfo = [System.Diagnostics.ProcessStartInfo]::new()
$processInfo.FileName = $resolvedExe
$processInfo.WorkingDirectory = $workingDirectory
$processInfo.Arguments = $argumentLine
$processInfo.UseShellExecute = $true

$process = [System.Diagnostics.Process]::Start($processInfo)

[pscustomobject]@{
    ProcessId = $process.Id
    FileName = $processInfo.FileName
    WorkingDirectory = $processInfo.WorkingDirectory
    Arguments = $processInfo.Arguments
    GuestCommandLine = $guestCommandLine
}
