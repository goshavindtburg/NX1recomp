param(
    [string]$DumpRoot = "D:\nx1-rexglue\tools\xenosrecomp_shader_dumps",
    [string]$OutputRoot = "D:\nx1-rexglue\tools\xenosrecomp_hlsl",
    [string]$XenosRecompExe = "D:\nx1-rexglue\XenosRecomp\out\build\x64-Clang-Release\XenosRecomp\XenosRecomp.exe",
    [string]$ShaderCommon = "D:\nx1-rexglue\XenosRecomp\XenosRecomp\shader_common.h"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $XenosRecompExe)) {
    throw "XenosRecomp executable not found: $XenosRecompExe"
}

if (!(Test-Path -LiteralPath $ShaderCommon)) {
    throw "XenosRecomp shader_common.h not found: $ShaderCommon"
}

if (!(Test-Path -LiteralPath $DumpRoot)) {
    Write-Host "No shader dump directory yet: $DumpRoot"
    Write-Host "Run NX1 once with dump_shaders enabled, then run this script again."
    exit 0
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$dumpRootFull = [System.IO.Path]::GetFullPath($DumpRoot)
$dumpRootPrefix = $dumpRootFull.TrimEnd('\') + '\'
$dumps = Get-ChildItem -LiteralPath $DumpRoot -Recurse -File |
    Where-Object { $_.Name.EndsWith(".ucode.bin.frag") -or $_.Name.EndsWith(".ucode.bin.vert") }

if ($dumps.Count -eq 0) {
    Write-Host "No raw ReXGlue ucode dumps found under $DumpRoot"
    Write-Host "Expected files like shader_<hash>.ucode.bin.frag or shader_<hash>.ucode.bin.vert."
    exit 0
}

$converted = 0
$failed = 0

foreach ($dump in $dumps) {
    $dumpFull = [System.IO.Path]::GetFullPath($dump.FullName)
    if (!$dumpFull.StartsWith($dumpRootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Dump path is outside dump root: $dumpFull"
    }
    $relative = $dumpFull.Substring($dumpRootPrefix.Length)
    $outPath = Join-Path $OutputRoot ($relative + ".hlsl")
    $outDir = Split-Path -Parent $outPath
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    & $XenosRecompExe $dump.FullName $outPath $ShaderCommon
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $outPath)) {
        $metadataPath = $dump.FullName + ".nx1_vfetch"
        if (Test-Path -LiteralPath $metadataPath) {
            Copy-Item -LiteralPath $metadataPath -Destination ($outPath + ".nx1_vfetch") -Force
        }
        ++$converted
    } else {
        ++$failed
        Write-Warning "Failed to decompile $($dump.FullName)"
    }
}

Write-Host "Converted $converted shader dump(s) to $OutputRoot"
if ($failed -ne 0) {
    Write-Warning "$failed shader dump(s) failed."
    exit 1
}
