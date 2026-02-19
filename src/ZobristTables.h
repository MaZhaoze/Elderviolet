#pragma once
#include <cstdint>
#include <random>

// 只放表，不要引用 Position，避免循环 include
struct ZobristTables {
    uint64_t psq[16][64]{};
    uint64_t sideKey = 0;
    uint64_t castleKey[16]{};
    uint64_t epKey[8]{};

    ZobristTables() {
        std::mt19937_64 rng(20260126ULL);
        auto R = [&]() { return rng(); };

        for (int p = 0; p < 16; p++)
            for (int s = 0; s < 64; s++)
                psq[p][s] = R();

        sideKey = R();
        for (int i = 0; i < 16; i++)
            castleKey[i] = R();
        for (int i = 0; i < 8; i++)
            epKey[i] = R();
    }
};

inline ZobristTables g_zob;