#pragma once

#include <algorithm>

namespace search {

inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

} // namespace search
