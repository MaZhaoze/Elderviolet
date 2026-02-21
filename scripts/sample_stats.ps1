param(
    [string]$Exe = ".\src\Elderviolet.exe",
    [int]$Runs = 8,
    [int]$Warmup = 2,
    [int]$MoveTime = 1000
)

$ErrorActionPreference = "Stop"

function Parse-KeyVals {
    param([string]$Line)
    $m = @{}
    if (-not $Line) { return $m }
    $parts = $Line -split '\s+'
    foreach ($p in $parts) {
        if ($p -match '=') {
            $kv = $p -split '='
            if ($kv.Length -eq 2) {
                $k = $kv[0].Trim()
                $v = $kv[1].Trim()
                if ($k -ne "") { $m[$k] = $v }
            }
        }
    }
    return $m
}

function Parse-DepthLine {
    param([string]$Line)
    $m = @{}
    if (-not $Line) { return $m }
    $tokens = $Line -split '\s+'
    for ($i = 0; $i -lt $tokens.Length - 1; $i++) {
        $k = $tokens[$i]
        if ($k -in @('depth','nodes','nps','hashfull','time')) {
            $m[$k] = $tokens[$i + 1]
        }
    }
    return $m
}

function To-Num($x) {
    if ($null -eq $x) { return $null }
    $y = 0.0
    if ([double]::TryParse([string]$x, [ref]$y)) { return $y }
    return $null
}

function Mean-Var {
    param([double[]]$Arr)
    if (-not $Arr -or $Arr.Count -eq 0) {
        return [pscustomobject]@{ Mean = $null; Var = $null }
    }
    $mean = ($Arr | Measure-Object -Average).Average
    $var = 0.0
    foreach ($x in $Arr) { $var += ($x - $mean) * ($x - $mean) }
    $var /= $Arr.Count
    return [pscustomobject]@{ Mean = [Math]::Round($mean, 3); Var = [Math]::Round($var, 3) }
}

function Run-Case {
    param([string]$Name, [string[]]$PosCmd)

    $rows = @()
    for ($i = 1; $i -le $Runs; $i++) {
        $cmd = @(
            "uci",
            "isready",
            "setoption name Threads value 1",
            "setoption name Hash value 64",
            "setoption name SearchStats value true",
            "ucinewgame"
        ) + $PosCmd + @(
            "go movetime $MoveTime",
            "quit"
        )

        $stdinPath = [System.IO.Path]::GetTempFileName()
        $stdoutPath = [System.IO.Path]::GetTempFileName()
        $stderrPath = [System.IO.Path]::GetTempFileName()
        Set-Content -Encoding UTF8 $stdinPath ($cmd -join "`n")

        $proc = Start-Process -FilePath $Exe -NoNewWindow -PassThru -RedirectStandardInput $stdinPath -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $proc.WaitForExit()

        $out = Get-Content $stdoutPath
        Remove-Item -Force $stdinPath, $stdoutPath, $stderrPath

        $depthLine = $out | Where-Object { $_ -match '^info depth' } | Select-Object -Last 1
        $rootLine = $out | Where-Object { $_ -match '^info string stats_root' } | Select-Object -Last 1
        $nodeLine = $out | Where-Object { $_ -match '^info string stats_node' } | Select-Object -Last 1
        $lmrLine = $out | Where-Object { $_ -match '^info string stats_lmr' } | Select-Object -Last 1
        $prLine = $out | Where-Object { $_ -match '^info string stats_prune' } | Select-Object -Last 1

        $d = Parse-DepthLine $depthLine
        $r = Parse-KeyVals $rootLine
        $n = Parse-KeyVals $nodeLine
        $l = Parse-KeyVals $lmrLine
        $p = Parse-KeyVals $prLine

        $rows += [pscustomobject]@{
            Run = $i
            depth = To-Num $d['depth']
            nodes = To-Num $d['nodes']
            nps = To-Num $d['nps']
            root_fh1 = To-Num $r['fh1']
            root_re = To-Num $r['re']
            tt_hit = To-Num $n['tt_hit']
            tt_cut = To-Num $n['tt_cut']
            ttm_first = To-Num $n['ttm_first']
            avgm_pv = To-Num $n['avgm_pv']
            avgm_cut = To-Num $n['avgm_cut']
            avgm_all = To-Num $n['avgm_all']
            lmr_red = To-Num $l['red']
            lmr_re = To-Num $l['re']
            lmr_rk = To-Num $l['rk']
            lmr_rc = To-Num $l['rc']
            lmr_rh = To-Num $l['rh']
            lmr_rl = To-Num $l['rl']
            lmr_rek = To-Num $l['rek']
            lmr_rec = To-Num $l['rec']
            lmr_reh = To-Num $l['reh']
            lmr_rel = To-Num $l['rel']
            null_t = To-Num $p['null_t']
            null_fh = To-Num $p['null_fh']
            null_vf = To-Num $p['null_vf']
            raz = To-Num $p['raz']
            rfp = To-Num $p['rfp']
            leg = To-Num $p['leg']
            legf = To-Num $p['legf']
            seem = To-Num $p['seem']
            seeq = To-Num $p['seeq']
            mk = To-Num $p['mk']
        }
    }

    $start = [Math]::Max(0, $Warmup)
    $slice = if ($rows.Count -gt $start) { $rows[$start..($rows.Count - 1)] } else { $rows }

    $keys = @(
        'depth','nodes','nps',
        'root_fh1','root_re',
        'tt_hit','tt_cut','ttm_first',
        'avgm_pv','avgm_cut','avgm_all',
        'lmr_red','lmr_re',
        'lmr_rk','lmr_rc','lmr_rh','lmr_rl',
        'lmr_rek','lmr_rec','lmr_reh','lmr_rel',
        'null_t','null_fh','null_vf','raz','rfp',
        'leg','legf','seem','seeq','mk'
    )
    $sum = [ordered]@{ case = $Name; runs = $Runs; warmup = $Warmup }
    foreach ($k in $keys) {
        $arr = @($slice | ForEach-Object { $_.$k } | Where-Object { $null -ne $_ })
        $mv = Mean-Var -Arr $arr
        $sum["$k`_mean"] = $mv.Mean
        $sum["$k`_var"] = $mv.Var
    }
    return [pscustomobject]$sum
}

$cases = @(
    @{ Name = "opening"; Pos = @("position startpos moves e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6") },
    @{ Name = "tactical"; Pos = @("position fen r2q1rk1/pp2bppp/2n2n2/2bp4/3P4/2N1PN2/PPQ1BPPP/R1B2RK1 w - - 0 10") },
    @{ Name = "endgame"; Pos = @("position fen 8/8/2k5/2p5/2P5/2K5/6R1/7r w - - 0 1") }
)

$out = @()
foreach ($c in $cases) {
    $out += Run-Case -Name $c.Name -PosCmd $c.Pos
}

$out | Format-Table -AutoSize
$out | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 .\bench\sample_stats_summary.json
