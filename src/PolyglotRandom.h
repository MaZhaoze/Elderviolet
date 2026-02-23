#pragma once

#include <array>
#include <cstdint>

// Fallback Polyglot random table generator.
// If you have a canonical Polyglot table in your branch, replace this file.
inline constexpr std::array<uint64_t, 781> kPolyRandom = []() {
    std::array<uint64_t, 781> a{};
    uint64_t x = 0x9d39247e33776d41ULL;
    for (size_t i = 0; i < a.size(); ++i) {
        x += 0x9e3779b97f4a7c15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        a[i] = z ^ (z >> 31);
    }
    return a;
}();
