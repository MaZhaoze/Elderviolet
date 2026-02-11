
<p align="center">
  <img src="logo.png" alt="Elderviolet logo" width="260">
</p>

<h1 align="center">Elderviolet</h1>

<p align="center">
  A UCI chess engine written in <b>pure C++</b>.
  <br>
  Focused on search strength, clean architecture, and informative UCI output.
</p>

<p align="center">
  <img src="https://img.shields.io/github/license/MaZhaoze/Elderviolet?style=for-the-badge" alt="License">
  <img src="https://img.shields.io/github/commits-since/MaZhaoze/Elderviolet/latest?style=for-the-badge" alt="Commits since latest">
  <img src="https://img.shields.io/badge/language-C++-6f42c1?style=for-the-badge" alt="Language">
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
