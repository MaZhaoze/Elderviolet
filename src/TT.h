#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

#include "types.h" // Move encoding.

namespace search {

// Transposition table (single replacement bucket).
enum TTFlag : uint8_t { TT_EXACT = 0, TT_ALPHA = 1, TT_BETA = 2 };

struct alignas(16) TTEntry {
    uint64_t key = 0;   // full zobrist key
    Move best = 0;      // best move
    int16_t score = 0;  // stored score (TT-adjusted)
    int8_t depth = -1;  // search depth in plies
    uint8_t flag = TT_EXACT;
};

// Keep TTEntry compact for predictable table sizing and cache behavior.
static_assert(sizeof(TTEntry) == 16, "TTEntry size changed; review layout/alignment.");

struct TT {
    std::vector<TTEntry> table;
    uint64_t mask = 0;

    TTEntry dummy; // Returned when the table is empty.

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
        // Power-of-two size: index is fast mask.
        return &table[size_t(key_) & mask];
    }

    inline void clear() {
        if (!table.empty()) {
            std::fill(table.begin(), table.end(), TTEntry{});
        }
    }
};

} // namespace search
