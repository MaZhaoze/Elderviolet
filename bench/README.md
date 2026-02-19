# Benchmarks

This repo includes a simple, reproducible UCI benchmark harness for single-threaded speed checks.

## Build

Use the exact build command recorded in `bench/build_cmd.txt`:

```powershell
Get-Content .\bench\build_cmd.txt | ForEach-Object { Invoke-Expression $_ }
```

## Run baseline

```powershell
.\scripts\bench.ps1
```

This writes:
- `bench/baseline_movetime.txt`
- `bench/baseline_depth.txt`

Each file includes full engine output for 5 runs, plus parsed metrics and averages (runs 2-5).

## Run a tagged benchmark (for optimization comparisons)

```powershell
.\scripts\bench.ps1 -Tag commitA
```

This writes:
- `bench/run_commitA_movetime.txt`
- `bench/run_commitA_depth.txt`

These tagged runs are ignored by git (see `.gitignore`).

## Parsing

The script extracts the last `info depth ...` line from each run and parses:
- `depth`
- `nodes`
- `nps`
- `hashfull`

Averages are computed over runs 2-5 to skip warm-up.
