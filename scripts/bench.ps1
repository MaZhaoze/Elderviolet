param(
    [string]$Exe = ".\Elderviolet.exe",
    [string]$Tag = "baseline"
)

$ErrorActionPreference = "Stop"

function Parse-InfoLine {
    param([string[]]$Lines)
    $info = $Lines | Where-Object { $_ -match '^info depth' } | Select-Object -Last 1
    if (-not $info) {
        return [pscustomobject]@{
            Depth = $null
            Nodes = $null
            Nps = $null
            Hashfull = $null
            Line = $null
        }
    }
    $tokens = $info -split '\s+'
    $map = @{}
    for ($i = 0; $i -lt $tokens.Length - 1; $i++) {
        $key = $tokens[$i]
        if ($key -in @('depth','nodes','nps','hashfull')) {
            $map[$key] = $tokens[$i + 1]
        }
    }
    return [pscustomobject]@{
        Depth = if ($map.ContainsKey('depth')) { [int]$map['depth'] } else { $null }
        Nodes = if ($map.ContainsKey('nodes')) { [int64]$map['nodes'] } else { $null }
        Nps = if ($map.ContainsKey('nps')) { [int64]$map['nps'] } else { $null }
        Hashfull = if ($map.ContainsKey('hashfull')) { [int]$map['hashfull'] } else { $null }
        Line = $info
    }
}

function Run-Bench {
    param(
        [string]$Mode,
        [int]$Value,
        [string]$OutFile
    )

    $runs = 5
    $warmup = 1
    $all = @()
    $metrics = @()

    for ($i = 1; $i -le $runs; $i++) {
        $cmd = @"
uci
isready
setoption name Threads value 1
setoption name Hash value 64
ucinewgame
position startpos
go $Mode $Value
quit
"@
        $output = @($cmd | & $Exe)
        $all += ,@{
            Run = $i
            Output = $output
        }
        $metrics += ,(Parse-InfoLine -Lines $output)
    }

    $avgDepth = $null
    $avgNodes = $null
    $avgNps = $null
    $avgHashfull = $null

    if ($metrics.Count -gt $warmup) {
        $slice = $metrics[$warmup..($metrics.Count - 1)]
        $depths = $slice | Where-Object { $_.Depth -ne $null } | ForEach-Object { $_.Depth }
        $nodes = $slice | Where-Object { $_.Nodes -ne $null } | ForEach-Object { $_.Nodes }
        $nps = $slice | Where-Object { $_.Nps -ne $null } | ForEach-Object { $_.Nps }
        $hashfull = $slice | Where-Object { $_.Hashfull -ne $null } | ForEach-Object { $_.Hashfull }

        if ($depths) { $avgDepth = [Math]::Round(($depths | Measure-Object -Average).Average, 2) }
        if ($nodes) { $avgNodes = [Math]::Round(($nodes | Measure-Object -Average).Average, 2) }
        if ($nps) { $avgNps = [Math]::Round(($nps | Measure-Object -Average).Average, 2) }
        if ($hashfull) { $avgHashfull = [Math]::Round(($hashfull | Measure-Object -Average).Average, 2) }
    }

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("Baseline: $Mode $Value; Threads=1; Hash=64; runs=$runs; warmup=$warmup")
    $lines.Add("")

    for ($i = 0; $i -lt $all.Count; $i++) {
        $lines.Add("Run $($all[$i].Run):")
        foreach ($line in $all[$i].Output) {
            $lines.Add([string]$line)
        }
        $lines.Add("")
        $m = $metrics[$i]
        $lines.Add("Parsed: depth=$($m.Depth) nodes=$($m.Nodes) nps=$($m.Nps) hashfull=$($m.Hashfull)")
        $lines.Add("")
    }

    $lines.Add("Average (runs 2-5): depth=$avgDepth nodes=$avgNodes nps=$avgNps hashfull=$avgHashfull")

    Set-Content -Encoding UTF8 $OutFile $lines

    return [pscustomobject]@{
        AverageDepth = $avgDepth
        AverageNodes = $avgNodes
        AverageNps = $avgNps
        AverageHashfull = $avgHashfull
        Runs = $all
        Metrics = $metrics
    }
}

$results = @{}
$prefix = if ($Tag -eq "baseline") { "baseline" } else { "run_$Tag" }
$results.Movetime = Run-Bench -Mode "movetime" -Value 2000 -OutFile ".\bench\${prefix}_movetime.txt"
$results.Depth = Run-Bench -Mode "depth" -Value 12 -OutFile ".\bench\${prefix}_depth.txt"

$results
