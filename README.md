<p align="center">
  <img src="logo.png" alt="Elderviolet logo" width="260">
</p>


<p align="center">
  A UCI chess engine written in <b>pure C++</b>.
  <br>
  Focused on search strength, clean architecture, and informative UCI output.
</p>



<p align="center">
  <!-- Base -->
  <img src="https://img.shields.io/github/license/MaZhaoze/Elderviolet?style=for-the-badge&color=6f42c1&labelColor=2b0a3d" alt="License">
  <img src="https://img.shields.io/github/stars/MaZhaoze/Elderviolet?style=for-the-badge&color=6f42c1&labelColor=2b0a3d" alt="Stars">
  <img src="https://img.shields.io/github/forks/MaZhaoze/Elderviolet?style=for-the-badge&color=6f42c1&labelColor=2b0a3d" alt="Forks">
  <img src="https://img.shields.io/github/watchers/MaZhaoze/Elderviolet?style=for-the-badge&color=6f42c1&labelColor=2b0a3d" alt="Watchers">
</p>

<p align="center">
  <!-- Activity -->
  <img src="https://img.shields.io/github/last-commit/MaZhaoze/Elderviolet?style=for-the-badge&color=4c6ef5&labelColor=2b0a3d" alt="Last commit">
  <img src="https://img.shields.io/github/commit-activity/m/MaZhaoze/Elderviolet?style=for-the-badge&color=4c6ef5&labelColor=2b0a3d" alt="Commit activity">
  <img src="https://img.shields.io/github/commits-since/MaZhaoze/Elderviolet/latest?style=for-the-badge&color=4c6ef5&labelColor=2b0a3d" alt="Commits since latest">
  <img src="https://img.shields.io/github/repo-size/MaZhaoze/Elderviolet?style=for-the-badge&color=4c6ef5&labelColor=2b0a3d" alt="Repo size">
</p>

<p align="center">
  <!-- Issues / PRs -->
  <img src="https://img.shields.io/github/issues/MaZhaoze/Elderviolet?style=for-the-badge&color=845ef7&labelColor=2b0a3d" alt="Issues">
  <img src="https://img.shields.io/github/issues-closed/MaZhaoze/Elderviolet?style=for-the-badge&color=845ef7&labelColor=2b0a3d" alt="Issues closed">
  <img src="https://img.shields.io/github/issues-pr/MaZhaoze/Elderviolet?style=for-the-badge&color=845ef7&labelColor=2b0a3d" alt="Open PRs">
  <img src="https://img.shields.io/github/issues-pr-closed/MaZhaoze/Elderviolet?style=for-the-badge&color=845ef7&labelColor=2b0a3d" alt="Closed PRs">
</p>

<p align="center">
  <!-- Releases / Downloads -->
  <img src="https://img.shields.io/github/v/release/MaZhaoze/Elderviolet?style=for-the-badge&color=20c997&labelColor=2b0a3d" alt="Release">
  <img src="https://img.shields.io/github/release-date/MaZhaoze/Elderviolet?style=for-the-badge&color=20c997&labelColor=2b0a3d" alt="Release date">
  <img src="https://img.shields.io/github/downloads/MaZhaoze/Elderviolet/total?style=for-the-badge&color=20c997&labelColor=2b0a3d" alt="Downloads">
</p>

<p align="center">
  <!-- Tech stack -->
  <img src="https://img.shields.io/badge/language-C%2B%2B-12b886?style=for-the-badge&labelColor=2b0a3d" alt="C++">
  <img src="https://img.shields.io/badge/build-manual-12b886?style=for-the-badge&labelColor=2b0a3d" alt="Build manual">
  <img src="https://img.shields.io/badge/domain-Chess%20Engine-12b886?style=for-the-badge&labelColor=2b0a3d" alt="Chess Engine">
  <img src="https://img.shields.io/badge/UCI-compatible-12b886?style=for-the-badge&labelColor=2b0a3d" alt="UCI">
</p>

<p align="center">
  <!-- Optional flex / identity -->
  <img src="https://img.shields.io/badge/status-active-f06595?style=for-the-badge&labelColor=2b0a3d" alt="Status">
  <img src="https://img.shields.io/badge/theme-Elderviolet-f06595?style=for-the-badge&labelColor=2b0a3d" alt="Theme">
  <img src="https://img.shields.io/badge/author-MaZhaoze-f06595?style=for-the-badge&labelColor=2b0a3d" alt="Author">
</p>
---

## Overview

Elderviolet is a UCI-compatible chess engine implemented in pure C++ (no external dependencies).  
It does not provide a GUI. To play or test the engine, connect it to a UCI GUI such as:

- Arena
- CuteChess / cutechess-cli
- BanksiaGUI
- ChessBase

---

## Features

Search & pruning:
- PVS
- Aspiration window
- LMR / LMP
- Null-move pruning
- Razor
- Futility pruning (incl. reverse futility)
- Mate distance pruning

Move ordering:
- Transposition table (TT) with mate score handling
- Killer moves
- History heuristic

Tactical filters:
- SEE pruning
- Full SEE fallback (`see_full.h`)

Parallel:
- Lazy SMP

---

## Build

This repository is header-heavy and uses `main.cpp` as the entry point.

### Windows (MSVC)

Example (single translation unit build):

```bat
cl /O2 /std:c++20 /EHsc main.cpp
````

### GCC / Clang

```bash
g++ -O3 -std=c++20 main.cpp -o Elderviolet
```

Tip: for local testing you can add native optimizations:

```bash
g++ -O3 -std=c++20 -march=native main.cpp -o Elderviolet
```

---

## Run (UCI)

Run the engine in a GUI, or from a terminal.

Example UCI session:

```text
uci
isready
ucinewgame
position startpos moves e2e4 e7e5
go depth 12
```

Common options (depending on your build) are typically:

* `Hash` (MB)
* `Threads`

---

## Project Layout

Key files in the repository root:

* `main.cpp` — program entry point
* `Engine.h` — engine coordination / high-level interface
* `uci.h` — UCI protocol handling and option parsing
* `Search.h` — search (PVS, pruning, SMP)
* `Evaluation.h` — evaluation
* `MoveGeneration.h` — move generation
* `Position.h` — board representation / state
* `Attack.h` — attack tables / helpers
* `TT.h` — transposition table
* `ZobristTables.h` — hashing keys / tables
* `see_full.h` — full SEE implementation
* `types.h` — core types / constants
* `logo.png` — project logo

---

## Notes

* The engine binaries (`*.exe`) in this folder are build artifacts and may be platform/CPU specific (e.g. AVX2).
* For strength testing, prefer running matches with `cutechess-cli` and fixed settings.

---

## License

MIT License (see `LICENSE`).
