#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

#include "types.h" // Move 在这里

namespace search {

// =====================
// TT
// =====================
enum TTFlag : uint8_t { TT_EXACT = 0, TT_ALPHA = 1, TT_BETA = 2 };

struct TTEntry {
    uint64_t key = 0;
    int16_t depth = -1;
    int16_t score = 0;
    uint8_t flag = TT_EXACT;
    Move best = 0;
};

struct TT {
    std::vector<TTEntry> table;
    uint64_t mask = 0;

    TTEntry dummy; // ✅ probe 永不返回 nullptr

    void resize_mb(int mb) {
        size_t bytes = size_t(std::max(1, mb)) * 1024ULL * 1024ULL;
        size_t n = std::max<size_t>(1, bytes / sizeof(TTEntry));
        size_t p2 = 1;
        while (p2 < n)
            p2 <<= 1;
        table.assign(p2, TTEntry{});
        mask = p2 - 1;
    }

    inline TTEntry* probe(uint64_t key_) {
        if (table.empty())
            return &dummy;
        return &table[size_t(key_) & mask];
    }

    inline void clear() {
        if (!table.empty()) {
            std::fill(table.begin(), table.end(), TTEntry{});
        }
    }
};

} // namespace search
