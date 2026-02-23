#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace search {

inline std::atomic<bool> g_stop{false};
inline std::atomic<int64_t> g_endTimeMs{0};
inline std::atomic<int64_t> g_softTimeMs{0};
inline std::atomic<uint64_t> g_nodes_total{0};

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void start_timer(int movetime_ms, bool infinite, int optimum_ms = 0) {
    g_stop.store(false, std::memory_order_relaxed);
    if (infinite || movetime_ms <= 0) {
        g_endTimeMs.store(0, std::memory_order_relaxed);
        g_softTimeMs.store(0, std::memory_order_relaxed);
        return;
    }
    const int64_t n = now_ms();
    g_endTimeMs.store(n + movetime_ms, std::memory_order_relaxed);
    const int soft = (optimum_ms > 0 ? optimum_ms : (movetime_ms * 7) / 10);
    g_softTimeMs.store(n + soft, std::memory_order_relaxed);
}

inline bool soft_time_up() {
    const int64_t s = g_softTimeMs.load(std::memory_order_relaxed);
    return s > 0 && now_ms() >= s;
}

inline void stop() {
    g_stop.store(true, std::memory_order_relaxed);
}

} // namespace search
