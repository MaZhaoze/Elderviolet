#pragma once
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <cmath>
#include <atomic>
#include <thread>
#include <memory>
#include <tuple>
#include <mutex>

#include "types.h"
#include "Position.h"
#include "ZobristTables.h"
#include "MoveGeneration.h"
#include "Attack.h"
#include "Evaluation.h"
#include "search/SearchConfig.h"
#include "search/SearchMoveFormat.h"
#include "search/SearchStats.h"
#include "search/SearchTiming.h"
#include "search/SearchUtils.h"
#include "TT.h"
#include "see_full.h"

namespace search {

// Search implementation: iterative deepening, PVS, pruning, and Lazy SMP.

// SearchInfo for UCI info output (header-only safe).
struct SearchInfo {
    int depth = 0;
    int seldepth = 0;
    uint64_t nodes = 0;
    int time_ms = 0;
    int nps = 0;
    int hashfull = 0; // 0..1000
};

inline int selDepth = 0;

// =====================================
// Color helpers
// =====================================
inline Color flip_color(Color c) {
    return (c == WHITE) ? BLACK : WHITE;
}
inline int color_index(Color c) {
    return (c == WHITE) ? 0 : 1;
}

// =====================================
// limits / result
// =====================================
struct Limits {
    int depth = 7;
    int movetime_ms = 0;
    int optimum_ms = 0;
    bool infinite = false;
};

struct Result {
    Move bestMove = 0;
    Move ponderMove = 0; // PV[1], or 0 if unknown
    int score = 0;
    uint64_t nodes = 0;
};

// =====================================
// misc helpers
// =====================================

// Approximate hash occupancy by sampling a fixed prefix.
inline int hashfull_permille_fallback(const TT& tt) {
    if (tt.table.empty())
        return 0;

    const size_t N = tt.table.size();
    const size_t SAMPLE = std::min<size_t>(N, 1u << 15);

    size_t filled = 0;
    for (size_t i = 0; i < SAMPLE; i++) {
        if (tt.table[i].key != 0)
            filled++;
    }
    return int((filled * 1000ULL) / SAMPLE);
}

// Thread-safe TT wrapper (striped locks).
struct SharedTT {
    TT tt;

    static constexpr int LOCKS = 4096; // power of two
    std::vector<std::mutex> locks;

    SharedTT() : locks(LOCKS) {}

    inline int lock_index(uint64_t key) const { return int((key ^ (key >> 32)) & (LOCKS - 1)); }

    // Lock-free read copy; may be slightly stale.
    inline bool probe_copy(uint64_t key, TTEntry& out) {
        TTEntry* e = tt.probe(key);
        if (!e || e->key != key)
            return false;
        // Read only hot fields needed by search, avoid whole-struct copy in hot path.
        out.key = e->key;
        out.best = e->best;
        out.score = e->score;
        out.depth = e->depth;
        out.flag = e->flag;
        return true;
    }

    // Locked store; replace on key mismatch or when depth is at least as deep.
    inline void store(uint64_t key, Move best, int16_t score, int16_t depth, uint8_t flag) {
        std::lock_guard<std::mutex> g(locks[lock_index(key)]);
        TTEntry* e = tt.probe(key);
        if (!e)
            return;

        if (e->key != key || depth >= e->depth) {
            e->key = key;
            e->best = best;
            e->score = score;
            e->depth = (depth > 127 ? 127 : (depth < -128 ? -128 : depth));
            e->flag = flag;
        }
    }

    inline void resize_mb(int mb) { tt.resize_mb(mb); }
    inline void clear() {
        for (auto& e : tt.table)
            e = TTEntry{};
    }
    inline int hashfull_permille() const { return hashfull_permille_fallback(tt); }
};

// Per-thread searcher state (history, killers, node counters).
struct alignas(64) Searcher {
    SharedTT* stt = nullptr;
    bool isMainSearchThread = false;
    int threadIndex = 0;
    int rootSplitOffset = 0;
    int rootSplitStride = 1;
    static constexpr int MAX_PLY = 512;
    static constexpr int KEY_STACK_MAX = 1024;

    Move killer[2][128]{};
    int history[2][64][64]{};

    Move countermove[64][64]{};
    int contHist[2][64][64][64][64]{};

    uint64_t nodes = 0;

    PruneStats ps;
    SearchStats ss;

    // Batch node counting to reduce atomic contention.
    uint64_t nodes_batch = 0;
    static constexpr uint64_t NODE_BATCH = 4096; // power of two
    static constexpr uint64_t NODE_BATCH_WORKER = 16384;
    uint32_t time_check_tick = 0;
    static constexpr uint32_t TIME_CHECK_MASK_NODE = 4095; // every 4096 probes
    static constexpr uint32_t TIME_CHECK_MASK_ROOT = 255;  // every 256 probes

    inline void batch_time_check_soft() {
        if (!isMainSearchThread)
            return;
        int64_t end = g_endTimeMs.load(std::memory_order_relaxed);
        if (end == 0)
            return;
        if (now_ms() >= end)
            g_stop.store(true, std::memory_order_relaxed);
    }

    inline void flush_nodes_batch() {
        if (nodes_batch) {
            g_nodes_total.fetch_add(nodes_batch, std::memory_order_relaxed);
            nodes_batch = 0;
        }
    }

    // Cheap stop/time probe: always read stop flag, but read clock sparsely.
    inline bool stop_or_time_up(bool rootNode) {
        if (g_stop.load(std::memory_order_relaxed))
            return true;
        // Worker threads follow the main thread stop flag only to avoid extra
        // clock probes on hot paths.
        if (!isMainSearchThread)
            return false;
        int64_t end = g_endTimeMs.load(std::memory_order_relaxed);
        if (end == 0)
            return false;

        const uint32_t mask = rootNode ? TIME_CHECK_MASK_ROOT : TIME_CHECK_MASK_NODE;
        if ((time_check_tick++ & mask) != 0)
            return false;

        if (collect_stats())
            ss.timeChecks++;

        if (now_ms() >= end) {
            if (isMainSearchThread)
                g_stop.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Count a node and periodically publish to the global counter.
    inline void add_node() {
        nodes++;

        nodes_batch++;

        const uint64_t batchLimit = isMainSearchThread ? NODE_BATCH : NODE_BATCH_WORKER;
        if (nodes_batch >= batchLimit) {
            g_nodes_total.fetch_add(nodes_batch, std::memory_order_relaxed);
            nodes_batch = 0;
            batch_time_check_soft();
        }
    }

    inline Undo do_move_counted(Position& pos, Move m, bool inQ, bool csOn) {
        if (csOn) {
            ss.makeCalls++;
            if (inQ)
                ss.makeQ++;
            else
                ss.makeMain++;
        }
        return pos.do_move(m);
    }

    inline Undo do_move_counted(Position& pos, Move m, bool inQ = false) {
        return do_move_counted(pos, m, inQ, collect_stats());
    }

    inline int see_quick_main(const Position& pos, Move m) {
        if (collect_stats())
            ss.seeCallsMain++;
        return see_quick(pos, m);
    }
    inline int see_full_main(const Position& pos, Move m) {
        if (collect_stats())
            ss.seeCallsMain++;
        return see_full(pos, m);
    }
    inline int see_quick_q(const Position& pos, Move m) {
        if (collect_stats())
            ss.seeCallsQ++;
        return see_quick(pos, m);
    }
    inline int see_full_q(const Position& pos, Move m) {
        if (collect_stats())
            ss.seeCallsQ++;
        return see_full(pos, m);
    }

    inline uint64_t compute_pinned_mask_for_side(const Position& pos, Color us) {
        const int ksq = pos.king_square(us);
        if ((unsigned)ksq >= 64u)
            return 0ULL;

        uint64_t pinned = 0ULL;
        const int kf = file_of(ksq);
        const int kr = rank_of(ksq);
        const Color them = flip_color(us);

        auto scan = [&](int df, int dr, bool diag) {
            int ff = kf + df;
            int rr = kr + dr;
            int ownSq = -1;
            while ((unsigned)ff < 8u && (unsigned)rr < 8u) {
                int sq = make_sq(ff, rr);
                Piece p = pos.board[sq];
                if (p != NO_PIECE) {
                    if (color_of(p) == us) {
                        if (ownSq == -1)
                            ownSq = sq;
                        else
                            break; // second own blocker -> no pin on this ray
                    } else {
                        PieceType pt = type_of(p);
                        bool slider = diag ? (pt == BISHOP || pt == QUEEN) : (pt == ROOK || pt == QUEEN);
                        if (slider && ownSq != -1 && color_of(p) == them)
                            pinned |= (1ULL << ownSq);
                        break;
                    }
                }
                ff += df;
                rr += dr;
            }
        };

        scan(+1, 0, false);
        scan(-1, 0, false);
        scan(0, +1, false);
        scan(0, -1, false);
        scan(+1, +1, true);
        scan(+1, -1, true);
        scan(-1, +1, true);
        scan(-1, -1, true);
        return pinned;
    }

    inline uint64_t pinned_mask_for_ply(const Position& pos, Color us, int plyCtx) {
        if ((unsigned)plyCtx >= (unsigned)MAX_PLY)
            return compute_pinned_mask_for_side(pos, us);
        if (!pinnedMaskValid[plyCtx]) {
            pinnedMaskCache[plyCtx] = compute_pinned_mask_for_side(pos, us);
            pinnedMaskValid[plyCtx] = true;
            if (collect_stats())
                ss.pinCalc++;
        }
        return pinnedMaskCache[plyCtx];
    }

    // Strictly safe SEE fast-path: if captured square has no enemy attackers after move,
    // exchange cannot continue, so SEE is non-negative for non-promo captures.
    inline bool see_fast_non_negative(Position& pos, Move m, bool inQ = false) {
        const int to = to_sq(m);
        Undo u = do_move_counted(pos, m, inQ);
        const bool safe = !attacks::is_square_attacked(pos, to, pos.side);
        pos.undo_move(m, u);
        if (safe && collect_stats())
            ss.seeFastSafe++;
        return safe;
    }

    struct PVLine {
        Move m[128]{};
        int len = 0;
    };

    Searcher() {
        for (int i = 0; i < MAX_PLY; i++) {
            plyMoves[i].reserve(256);
            plyScores[i].reserve(256);
            plyOrder[i].reserve(256);
            plyQList[i].reserve(256);
        }

    }

    inline void bind(SharedTT* shared) { stt = shared; }
    inline void set_thread_index(int idx) { threadIndex = std::max(0, idx); }
    inline void set_root_split(int offset, int stride) {
        rootSplitOffset = std::max(0, offset);
        rootSplitStride = std::max(1, stride);
    }

    inline uint64_t key_of(const Position& pos) const { return pos.zobKey; }

    uint64_t keyStack[KEY_STACK_MAX]{};
    int keyPly = 0;
    int staticEvalStack[MAX_PLY]{};
    uint64_t pinnedMaskCache[MAX_PLY]{};
    bool pinnedMaskValid[MAX_PLY]{};
    int kingSqCache[MAX_PLY]{};
    bool inCheckCache[MAX_PLY]{};
    bool inCheckCacheValid[MAX_PLY]{};

    std::vector<Move> plyMoves[MAX_PLY];
    std::vector<int> plyScores[MAX_PLY];
    std::vector<int> plyOrder[MAX_PLY];

    struct QNode {
        Move m;
        int key;
        bool cap;
        bool promo;
        bool check;
    };
    std::vector<QNode> plyQList[MAX_PLY];

    // Gravity-style history update: big bonuses have diminishing effect when value is saturated.
    inline void update_stat(int& v, int bonus, int cap = 300000) {
        bonus = clampi(bonus, -cap, cap);
        v += bonus - int((1LL * std::abs(bonus) * v) / cap);
        v = clampi(v, -cap, cap);
    }

    inline bool is_capture(const Position& pos, Move m) const {
        if (flags_of(m) & MF_EP)
            return true;
        int to = to_sq(m);
        return pos.board[to] != NO_PIECE;
    }

    inline int mvv_lva(Piece victim, Piece attacker) {
        static const int V[7] = {0, 100, 320, 330, 500, 900, 0};
        int vv = (victim == NO_PIECE) ? 0 : V[type_of(victim)];
        int aa = (attacker == NO_PIECE) ? 0 : V[type_of(attacker)];
        return vv * 10 - aa;
    }

    inline int piece_cp(Piece p) const {
        static const int V[7] = {0, 100, 320, 330, 500, 900, 0};
        return (p == NO_PIECE) ? 0 : V[type_of(p)];
    }

    inline int count_safe_retreats(const Position& pos, int from) {
        Piece mover = pos.board[from];
        if (mover == NO_PIECE)
            return 0;
        const Color us = pos.side;
        const Color them = flip_color(us);
        auto safe_empty = [&](int to) {
            if ((unsigned)to >= 64u || pos.board[to] != NO_PIECE)
                return false;
            int en = attacks::attackers_to_count(pos, to, them);
            if (en == 0)
                return true;
            int my = attacks::attackers_to_count(pos, to, us);
            return my >= en;
        };

        int cnt = 0;
        int f0 = file_of(from), r0 = rank_of(from);
        PieceType pt = type_of(mover);

        if (pt == KNIGHT) {
            static const int df[8] = {1, 2, 2, 1, -1, -2, -2, -1};
            static const int dr[8] = {2, 1, -1, -2, -2, -1, 1, 2};
            for (int i = 0; i < 8; i++) {
                int nf = f0 + df[i], nr = r0 + dr[i];
                if ((unsigned)nf < 8u && (unsigned)nr < 8u && safe_empty(make_sq(nf, nr)))
                    cnt++;
            }
            return cnt;
        }

        auto scan_dirs = [&](const int* dirs) {
            for (int i = 0; i < 4; i++) {
                int df = dirs[2 * i], dr = dirs[2 * i + 1];
                int f = f0 + df, r = r0 + dr;
                while ((unsigned)f < 8u && (unsigned)r < 8u) {
                    int to = make_sq(f, r);
                    if (pos.board[to] != NO_PIECE)
                        break;
                    if (safe_empty(to))
                        cnt++;
                    f += df;
                    r += dr;
                }
            }
        };

        static const int DIR_B[8] = {1, 1, 1, -1, -1, 1, -1, -1};
        static const int DIR_R[8] = {1, 0, -1, 0, 0, 1, 0, -1};
        if (pt == BISHOP || pt == QUEEN)
            scan_dirs(DIR_B);
        if (pt == ROOK || pt == QUEEN)
            scan_dirs(DIR_R);
        return cnt;
    }

    inline int see_quick(const Position& pos, Move m) {
        int from = from_sq(m);
        int to = to_sq(m);

        Piece a = pos.board[from];
        Piece v = pos.board[to];

        static const int V[7] = {0, 100, 320, 330, 500, 900, 0};

        int av = V[type_of(a)];
        int vv = (v == NO_PIECE ? 0 : V[type_of(v)]);

        if (flags_of(m) & MF_EP)
            vv = 100;
        if (promo_of(m))
            vv += 800;

        return vv - av;
    }

    inline bool force_full_see_for_trade(Piece attacker, Piece victim) const {
        if (attacker == NO_PIECE)
            return false;
        const PieceType at = type_of(attacker);
        const PieceType vt = (victim == NO_PIECE) ? PAWN : type_of(victim);
        if (at == ROOK || at == QUEEN || vt == ROOK || vt == QUEEN)
            return true;
        // For equal-or-worse first captures, force full SEE to avoid shallow quick-SEE mistakes.
        return piece_cp(attacker) >= piece_cp(victim);
    }

    #include "search/SearcherReductionHelpers.inl"

    #include "search/SearcherMoveOrdering.inl"

    #include "search/SearcherQSearch.inl"

    #include "search/SearcherPruningHelpers.inl"

    #include "search/SearcherLegality.inl"

    #include "search/SearcherPVUtils.inl"

    #include "search/SearcherNegamax.inl"

    #include "search/SearcherThink.inl"
};

// Thread pool / Lazy SMP globals.
inline std::atomic<int> g_threads{1};
inline int g_hash_mb = 64;

inline std::unique_ptr<SharedTT> g_shared_tt;

inline std::vector<std::unique_ptr<Searcher>> g_pool_owner;
inline std::vector<Searcher*> g_pool;

inline int threads() {
    return g_threads.load(std::memory_order_relaxed);
}

// Lazy SMP scales poorly on very short limits due to duplicated work and TT
// contention. Use fewer effective threads for short searches to preserve depth.
inline int effective_threads_for_limits(const Limits& lim, int requested) {
    int n = std::max(1, requested);
    if (n <= 1)
        return 1;
    if (lim.infinite)
        return n;
    if (lim.movetime_ms <= 0)
        return n;

    const int ms = lim.movetime_ms;
    if (ms <= 1200)
        return 1;
    if (ms <= 2500)
        return std::min(n, 2);
    if (ms <= 5000)
        return std::min(n, 4);
    if (ms <= 12000)
        return std::min(n, 8);
    return n;
}

inline void ensure_pool() {
    if (!g_shared_tt) {
        g_shared_tt = std::make_unique<SharedTT>();
        g_shared_tt->resize_mb(std::max(1, g_hash_mb));
    }

    if (g_pool.empty()) {
        g_pool_owner.clear();
        g_pool.clear();

        g_pool_owner.emplace_back(std::make_unique<Searcher>());
        g_pool_owner.back()->bind(g_shared_tt.get());
        g_pool_owner.back()->set_thread_index(0);
        g_pool.push_back(g_pool_owner.back().get());

        g_threads.store(1, std::memory_order_relaxed);
    }
}

inline void set_threads(int n) {
    ensure_pool();

    n = std::max(1, std::min(256, n));
    g_threads.store(n, std::memory_order_relaxed);

    g_pool_owner.clear();
    g_pool_owner.reserve(n);
    g_pool.clear();
    g_pool.reserve(n);

    for (int i = 0; i < n; i++) {
        g_pool_owner.emplace_back(std::make_unique<Searcher>());
        g_pool_owner.back()->bind(g_shared_tt.get());
        g_pool_owner.back()->set_thread_index(i);
        g_pool.push_back(g_pool_owner.back().get());
    }
}

// Public search API (single-thread or Lazy SMP).
inline Result think(Position& pos, const Limits& lim) {
    ensure_pool();

    g_nodes_total.store(0, std::memory_order_relaxed);
    start_timer(lim.movetime_ms, lim.infinite, lim.optimum_ms);

    const int requestedThreads = threads();
    const int n = effective_threads_for_limits(lim, requestedThreads);
    if (n <= 1) {
        Result r = g_pool[0]->think(pos, lim, true);
        r.nodes = g_nodes_total.load(std::memory_order_relaxed);
        return r;
    }

    std::vector<std::thread> workers;
    workers.reserve(n - 1);

    // Worker threads search independent copies of the root position.
    // Split root move space across workers to reduce duplicated root work.
    for (int i = 1; i < n; i++) {
        g_pool[i]->set_root_split(i - 1, n - 1);
        Position pcopy = pos;
        workers.emplace_back([i, pcopy, lim]() mutable { g_pool[i]->think(pcopy, lim, false); });
    }

    g_pool[0]->set_root_split(0, 1);
    Result mainRes = g_pool[0]->think(pos, lim, true);

    stop();
    for (auto& th : workers)
        th.join();

    mainRes.nodes = g_nodes_total.load(std::memory_order_relaxed);
    return mainRes;
}

inline void set_hash_mb(int mb) {
    ensure_pool();
    g_hash_mb = std::max(1, mb);
    g_shared_tt->resize_mb(g_hash_mb);
}

inline void clear_tt() {
    ensure_pool();
    g_shared_tt->clear();
}

} // namespace search










