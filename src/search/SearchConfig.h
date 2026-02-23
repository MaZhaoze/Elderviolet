#pragma once

namespace search {

struct SearchConfig {
    int rootOrderK = 16;
};

inline SearchConfig g_params{};
inline constexpr int INF = 32000;
inline constexpr int MATE = 30000;

} // namespace search
