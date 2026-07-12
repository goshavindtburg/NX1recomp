param(
    [Parameter(Mandatory = $true)]
    [string]$MapPath,

    [Parameter(Mandatory = $true)]
    [string]$OutPath,

    [string]$NamePrefix = "NX1",

    [int]$MinimumSize = 4
)

$ErrorActionPreference = "Stop"

function Convert-ToIdentifier {
    param([string]$Name)

    $sanitized = [regex]::Replace($Name, '[^A-Za-z0-9_]', '_')
    $sanitized = [regex]::Replace($sanitized, '_+', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($sanitized)) {
        $sanitized = "sub"
    }
    if ($sanitized[0] -match '[0-9]') {
        $sanitized = "_$sanitized"
    }
    if (-not [string]::IsNullOrEmpty($NamePrefix) -and -not $sanitized.StartsWith($NamePrefix)) {
        $sanitized = "$NamePrefix$sanitized"
    }
    return $sanitized
}

$sectionRegex = '^\s*(?<seg>[0-9A-Fa-f]{4}):(?<off>[0-9A-Fa-f]{8})\s+(?<len>[0-9A-Fa-f]+)H\s+(?<name>\S+)\s+(?<class>CODE|DATA)\s*$'
$symbolRegex = '^\s*(?<seg>[0-9A-Fa-f]{4}):(?<off>[0-9A-Fa-f]{8})\s+(?<name>\S+)\s+(?<addr>[0-9A-Fa-f]{8})\s+(?<kind>\S+)(?:\s+(?<rest>.*))?$'

$sections = @{}
$rawSymbols = New-Object System.Collections.Generic.List[object]

foreach ($line in Get-Content -LiteralPath $MapPath) {
    $sectionMatch = [regex]::Match($line, $sectionRegex)
    if ($sectionMatch.Success -and $sectionMatch.Groups['class'].Value -eq 'CODE') {
        $seg = $sectionMatch.Groups['seg'].Value.ToUpperInvariant()
        $sections[$seg] = [pscustomobject]@{
            Segment = $seg
            Offset = [Convert]::ToUInt32($sectionMatch.Groups['off'].Value, 16)
            Length = [Convert]::ToUInt32($sectionMatch.Groups['len'].Value, 16)
            Name = $sectionMatch.Groups['name'].Value
        }
        continue
    }

    $symbolMatch = [regex]::Match($line, $symbolRegex)
    if (-not $symbolMatch.Success) {
        continue
    }
    if ($symbolMatch.Groups['kind'].Value -ne 'f') {
        continue
    }

    $seg = $symbolMatch.Groups['seg'].Value.ToUpperInvariant()
    if (-not $sections.ContainsKey($seg)) {
        continue
    }

    $rawSymbols.Add([pscustomobject]@{
        Segment = $seg
        Offset = [Convert]::ToUInt32($symbolMatch.Groups['off'].Value, 16)
        Address = [Convert]::ToUInt32($symbolMatch.Groups['addr'].Value, 16)
        RawName = $symbolMatch.Groups['name'].Value
    })
}

$symbolsByAddress = @{}
foreach ($symbol in $rawSymbols | Sort-Object Address, RawName) {
    $key = ('0x{0:X8}' -f $symbol.Address)
    if (-not $symbolsByAddress.ContainsKey($key)) {
        $symbolsByAddress[$key] = $symbol
    }
}

$symbols = @($symbolsByAddress.Values | Sort-Object Address)
$usedNames = @{}
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("# Generated from $([System.IO.Path]::GetFileName($MapPath)).")
$lines.Add("# Source: $([System.IO.Path]::GetFullPath($MapPath))")
$lines.Add("")
$lines.Add("[functions]")

for ($i = 0; $i -lt $symbols.Count; $i++) {
    $symbol = $symbols[$i]
    $section = $sections[$symbol.Segment]
    $sectionEnd = $symbol.Address + [Math]::Max(0, ($section.Offset + $section.Length) - $symbol.Offset)
    $nextInSection = $null
    for ($j = $i + 1; $j -lt $symbols.Count; $j++) {
        if ($symbols[$j].Segment -eq $symbol.Segment) {
            $nextInSection = $symbols[$j]
            break
        }
    }

    $end = if ($null -ne $nextInSection) { $nextInSection.Address } else { $sectionEnd }
    $size = [Math]::Max($MinimumSize, [int]($end - $symbol.Address))
    $name = Convert-ToIdentifier $symbol.RawName

    if ($usedNames.ContainsKey($name)) {
        $name = ('{0}_{1:X8}' -f $name, $symbol.Address)
    }
    $usedNames[$name] = $true

    $lines.Add(('0x{0:X8} = {{ name = "{1}", size = 0x{2:X} }}' -f $symbol.Address, $name, $size))
}

$outDir = Split-Path -Parent $OutPath
if (-not [string]::IsNullOrEmpty($outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

[System.IO.File]::WriteAllLines($OutPath, $lines, [System.Text.UTF8Encoding]::new($false))
Write-Host "Wrote $($symbols.Count) function entries to $OutPath"
