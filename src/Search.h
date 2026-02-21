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
#include "TT.h"
#include "see_full.h"

namespace search {

// Search implementation: iterative deepening, PVS, pruning, and Lazy SMP.

inline bool is_white_piece_s(Piece p) {
    int v = (int)p;
    return v >= 1 && v <= 6;
}
inline bool is_black_piece_s(Piece p) {
    int v = (int)p;
    return v >= 9 && v <= 14;
}

// Simple time allocation heuristic for clock mode.
static inline int compute_think_time_ms(int mytime_ms, int myinc_ms, int movestogo) {
    if (mytime_ms <= 0)
        return -1;

    if (movestogo > 0) {
        int base = mytime_ms / std::max(1, movestogo);
        int inc_part = (myinc_ms * 8) / 10;
        int t = base + inc_part;

        t = std::max(10, t);
        t = std::min(t, mytime_ms / 2);
        return t;
    }

    int t = mytime_ms / 25 + (myinc_ms * 8) / 10;

    t = std::max(10, t);
    t = std::min(t, mytime_ms / 2);
    return t;
}

// Lightweight sanity check used by search and TT probing.
inline bool move_sane_basic(const Position& pos, Move m) {
    if (!m)
        return false;

    int from = from_sq(m), to = to_sq(m);
    if ((unsigned)from >= 64u || (unsigned)to >= 64u)
        return false;

    Piece pc = pos.board[from];
    if (pc == NO_PIECE)
        return false;

    // from must be side-to-move piece
    if (pos.side == WHITE) {
        if (!is_white_piece_s(pc))
            return false;
    } else {
        if (!is_black_piece_s(pc))
            return false;
    }

    // no self-capture (except EP)
    if (!(flags_of(m) & MF_EP)) {
        Piece cap = pos.board[to];
        if (cap != NO_PIECE) {
            if (pos.side == WHITE) {
                if (is_white_piece_s(cap))
                    return false;
            } else {
                if (is_black_piece_s(cap))
                    return false;
            }
        }
    }
    return true;
}

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
// constants
// =====================================
static constexpr int INF = 30000;
static constexpr int MATE = 29000;

// Search parameter governance: all pruning/reduction knobs are centralized here.
struct SearchParams {
    int nodeOrderK = 12;
    int rootOrderK = 10;

    bool ttPvConservative = true;

    bool enableRazoring = true;
    int razorDepthMax = 2;
    int razorMarginD1 = 220;
    int razorMarginD2 = 320;
    int razorImprovingBonus = 20;

    bool enableRfp = true;
    int rfpDepthMax = 3;
    int rfpBase = 120;
    int rfpPerDepth = 90;
    int rfpImprovingBonus = 40;

    bool enableIIR = true;
    int iirMinDepth = 6;
    int iirReduce = 1;

    bool enableNullMove = true;
    int nullMinDepth = 3;
    int nullBase = 3;
    int nullDepthDiv = 6;
    int nullMateGuard = 256;

    bool enableQuietFutility = true;
    int quietFutilityDepthMax = 2;
    int quietFutilityD1 = 190;
    int quietFutilityD2 = 290;
    int quietFutilityImprovingBonus = 40;

    bool enableQuietLimit = true;
    int quietLimitDepthMax = 2;
    int quietLimitD1 = 5;
    int quietLimitD2 = 8;

    bool enableCapSeePrune = true;
    int capSeeDepthMax = 4;
    int capSeeQuickFullTrigger = -200;
    int capSeeFullCut = -120;
    int capSeeQuickCut = -120;

    bool enableLmr = true;
    int lmrMinDepth = 3;
    int lmrMove1 = 4;
    int lmrMove2 = 10;
    int lmrMove3 = 14;
    int lmrDepthForMove3 = 7;
    int lmrHistoryLow = 2000;
    int lmrHistoryHigh = 60000;
    int lmrBucketHigh = 6000; // stats-only bucket threshold on combined quiet score
};

inline SearchParams g_params{};
inline std::atomic<bool> g_collect_stats{false};

inline void set_collect_stats(bool on) {
    g_collect_stats.store(on, std::memory_order_relaxed);
}
inline bool collect_stats() {
    return g_collect_stats.load(std::memory_order_relaxed);
}

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
// global stop/time (MULTI-THREAD SAFE)
// =====================================
inline std::atomic<bool> g_stop{false};
inline std::atomic<int64_t> g_endTimeMs{0};
inline std::atomic<uint64_t> g_nodes_total{0};

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void stop() {
    g_stop.store(true, std::memory_order_relaxed);
}

inline void start_timer(int movetime_ms, bool infinite) {
    g_stop.store(false, std::memory_order_relaxed);
    if (infinite || movetime_ms <= 0)
        g_endTimeMs.store(0, std::memory_order_relaxed);
    else
        g_endTimeMs.store(now_ms() + (int64_t)movetime_ms, std::memory_order_relaxed);
}

inline bool time_up() {
    if (g_stop.load(std::memory_order_relaxed))
        return true;
    int64_t end = g_endTimeMs.load(std::memory_order_relaxed);
    if (end == 0)
        return false;
    return now_ms() >= end;
}

// =====================================
// limits / result
// =====================================
struct Limits {
    int depth = 7;
    int movetime_ms = 0;
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
inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

inline void print_score_uci(int score) {
    const int MATE_SCORE = MATE;

    if (std::abs(score) >= MATE_SCORE - 1000) {
        int mateIn = (MATE_SCORE - std::abs(score) + 1) / 2;
        if (score < 0)
            mateIn = -mateIn;
        std::cout << "score mate " << mateIn;
    } else {
        std::cout << "score cp " << score;
    }
}

// =====================================
// TT mate distance fix
// =====================================
inline int to_tt_score(int s, int ply) {
    if (s > MATE - 256)
        return s + ply;
    if (s < -MATE + 256)
        return s - ply;
    return s;
}
inline int from_tt_score(int s, int ply) {
    if (s > MATE - 256)
        return s - ply;
    if (s < -MATE + 256)
        return s + ply;
    return s;
}

// =====================================
// move->uci
// =====================================
inline char file_char(int f) {
    return char('a' + f);
}
inline char rank_char(int r) {
    return char('1' + r);
}

inline std::string move_to_uci(Move m) {
    int f = from_sq(m), t = to_sq(m);

    char buf[6];
    buf[0] = file_char(file_of(f));
    buf[1] = rank_char(rank_of(f));
    buf[2] = file_char(file_of(t));
    buf[3] = rank_char(rank_of(t));
    int len = 4;

    int pr = promo_of(m);
    if (pr) {
        char pc = 'q';
        if (pr == 1)
            pc = 'n';
        else if (pr == 2)
            pc = 'b';
        else if (pr == 3)
            pc = 'r';
        else
            pc = 'q';
        buf[4] = pc;
        len = 5;
    }

    return std::string(buf, buf + len);
}

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
        out = *e;
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
            e->depth = depth;
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
struct Searcher {
    SharedTT* stt = nullptr;

    Move killer[2][128]{};
    int history[2][64][64]{};

    Move countermove[64][64]{};
    int contHist[2][64][64][64][64]{};

    uint64_t nodes = 0;

    struct PruneStats {
        uint64_t razorPrune = 0;
        uint64_t rfpPrune = 0;
        uint64_t quietFutility = 0;
        uint64_t quietLimit = 0;
        uint64_t capSeePrune = 0;
        uint64_t iirApplied = 0;
        uint64_t lmrApplied = 0;
        uint64_t betaCutoff = 0;
        inline void clear() { *this = PruneStats{}; }
    } ps;

    struct SearchStats {
        uint64_t nodePv = 0;
        uint64_t nodeCut = 0;
        uint64_t nodeAll = 0;
        uint64_t nodeByType[3]{};
        uint64_t legalByType[3]{};
        uint64_t ttProbe = 0;
        uint64_t ttHit = 0;
        uint64_t ttCut = 0;
        uint64_t ttBest = 0;
        uint64_t ttMoveAvail = 0;
        uint64_t ttMoveFirst = 0;
        uint64_t firstMoveTried = 0;
        uint64_t firstMoveFailHigh = 0;
        uint64_t lmrTried = 0;
        uint64_t lmrResearched = 0;
        uint64_t lmrReducedByBucket[4]{};   // killer/counter/quiet-high/quiet-low
        uint64_t lmrResearchedByBucket[4]{}; // killer/counter/quiet-high/quiet-low
        uint64_t nullTried = 0;
        uint64_t nullCut = 0;
        uint64_t nullVerifyTried = 0;
        uint64_t nullVerifyFail = 0;
        uint64_t lmpSkip = 0;
        uint64_t futilitySkip = 0;
        uint64_t totalLegalTried = 0;
        uint64_t moveLoopNodes = 0;
        uint64_t rootIters = 0;
        uint64_t rootFirstBestOrCut = 0;
        uint64_t rootBestSrc[5]{}; // tt/cap/killer/counter/quiet
        uint64_t rootPvsReSearch = 0;
        uint64_t rootLmrReSearch = 0;
        uint64_t rootNonFirstTried = 0;
        uint64_t aspFail = 0;
        uint64_t proxyReversalAfterNull = 0;
        uint64_t proxyReversalAfterRfp = 0;
        uint64_t proxyReversalAfterRazor = 0;
        uint64_t timeChecks = 0;
        inline void clear() { *this = SearchStats{}; }
    } ss;

    struct NodeContext {
        enum NodeType : uint8_t { PV = 0, CUT = 1, ALL = 2 };
        NodeType nodeType = PV;
        int depth = 0;
        int ply = 0;
        bool inCheck = false;
        int staticEval = -INF;
        bool improving = false;
        bool ttHit = false;
        int ttDepth = -1;
        uint8_t ttBound = TT_ALPHA;
        uint8_t ttConfidence = 0; // 0..3
        uint8_t endgameRisk = 0;  // 0..2
    };

    // Batch node counting to reduce atomic contention.
    uint64_t nodes_batch = 0;
    static constexpr uint64_t NODE_BATCH = 4096; // power of two
    uint32_t time_check_tick = 0;
    static constexpr uint32_t TIME_CHECK_MASK_NODE = 4095; // every 4096 probes
    static constexpr uint32_t TIME_CHECK_MASK_ROOT = 255;  // every 256 probes

    inline void batch_time_check_soft() {
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
        int64_t end = g_endTimeMs.load(std::memory_order_relaxed);
        if (end == 0)
            return false;

        const uint32_t mask = rootNode ? TIME_CHECK_MASK_ROOT : TIME_CHECK_MASK_NODE;
        if ((time_check_tick++ & mask) != 0)
            return false;

        if (collect_stats())
            ss.timeChecks++;

        if (now_ms() >= end) {
            g_stop.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Count a node and periodically publish to the global counter.
    inline void add_node() {
        nodes++;

        nodes_batch++;

        if (nodes_batch == NODE_BATCH) {
            g_nodes_total.fetch_add(NODE_BATCH, std::memory_order_relaxed);
            nodes_batch = 0;
            batch_time_check_soft();
        }
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

    inline uint64_t key_of(const Position& pos) const { return pos.zobKey; }

    uint64_t keyStack[256]{};
    int keyPly = 0;
    int staticEvalStack[256]{};

    static constexpr int MAX_PLY = 128;

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

    inline bool is_capture(const Position& pos, Move m) {
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

    inline int razor_margin(int depth, bool improving) const {
        int margin = (depth <= 1) ? g_params.razorMarginD1 : g_params.razorMarginD2;
        if (improving)
            margin += g_params.razorImprovingBonus;
        return margin;
    }

    inline int rfp_margin(int depth, bool improving) const {
        int margin = g_params.rfpBase + g_params.rfpPerDepth * depth;
        if (improving)
            margin += g_params.rfpImprovingBonus;
        return margin;
    }

    inline int quiet_futility_margin(int depth, bool improving) const {
        int margin = (depth <= 1) ? g_params.quietFutilityD1 : g_params.quietFutilityD2;
        if (improving)
            margin += g_params.quietFutilityImprovingBonus;
        return margin;
    }

    inline int quiet_limit_for_depth(int depth) const {
        return (depth <= 1) ? g_params.quietLimitD1 : g_params.quietLimitD2;
    }

    inline void update_quiet_history(Color us, int prevFrom, int prevTo, int from, int to, int depth, bool good) {
        int ci = color_index(us);
        const int sign = good ? 1 : -1;
        const int hBonus = depth * depth * (good ? 16 : 2);
        const int cBonus = depth * depth * (good ? 12 : 1);

        update_stat(history[ci][from][to], sign * hBonus);
        if ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u)
            update_stat(contHist[ci][prevFrom][prevTo][from][to], sign * cBonus);
    }

    inline int compute_lmr_reduction(int depth, int legalMovesSearched, bool inCheck, bool isQuiet, bool improving,
                                     bool isPvNode, Color us, int from, int to) {
        if (!g_params.enableLmr || depth < g_params.lmrMinDepth || inCheck || !isQuiet)
            return 0;

        int reduction = 1;
        if (legalMovesSearched > g_params.lmrMove1)
            reduction++;
        if (legalMovesSearched > g_params.lmrMove2)
            reduction++;
        if (depth >= g_params.lmrDepthForMove3 && legalMovesSearched > g_params.lmrMove3)
            reduction++;
        if (!improving)
            reduction++;
        if (isPvNode)
            reduction = std::max(0, reduction - 1);

        int ci = color_index(us);
        int h = history[ci][from][to] / 2;
        if (h < g_params.lmrHistoryLow)
            reduction++;
        if (h > g_params.lmrHistoryHigh)
            reduction = std::max(0, reduction - 1);

        reduction = std::min(reduction, depth - 2);
        return std::max(0, reduction);
    }

    inline int node_type_index(NodeContext::NodeType t) const {
        if (t == NodeContext::PV)
            return 0;
        if (t == NodeContext::CUT)
            return 1;
        return 2;
    }

    inline int lmr_bucket(bool isKiller, bool isCounter, int quietScore) const {
        // Legacy bucket helper (kept for compatibility in non-stats callsites).
        if (isKiller)
            return 0;
        if (isCounter)
            return 1;
        if (quietScore >= g_params.lmrHistoryLow)
            return 2;
        return 3;
    }

    inline int quiet_bucket_score(Color us, int from, int to, int prevFrom, int prevTo) const {
        int ci = color_index(us);
        int sc = history[ci][from][to] / 2;
        if ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u)
            sc += contHist[ci][prevFrom][prevTo][from][to] / 2;
        return sc;
    }

    inline int lmr_bucket_refined(bool isKiller, bool isCounter, bool recapture, bool givesCheck, int qScore) const {
        // 0 killer, 1 counter, 2 quiet-high/protected, 3 quiet-low
        if (isKiller)
            return 0;
        if (isCounter)
            return 1;
        if (recapture || givesCheck)
            return 2;
        if (qScore >= g_params.lmrBucketHigh)
            return 2;
        return 3;
    }

    inline NodeContext make_node_context(const Position& pos, int depth, int alpha, int beta, int ply, bool inCheck,
                                         int staticEval, bool improving, bool ttHit, const TTEntry& te) const {
        NodeContext ctx{};
        ctx.depth = depth;
        ctx.ply = ply;
        ctx.inCheck = inCheck;
        ctx.staticEval = staticEval;
        ctx.improving = improving;
        ctx.ttHit = ttHit;
        if (beta - alpha > 1)
            ctx.nodeType = NodeContext::PV;
        else
            ctx.nodeType = (staticEval >= beta ? NodeContext::CUT : NodeContext::ALL);

        if (ttHit) {
            ctx.ttDepth = te.depth;
            ctx.ttBound = te.flag;
            int conf = 0;
            if (te.depth >= depth)
                conf++;
            if (te.flag == TT_EXACT)
                conf += 2;
            else if (te.flag == TT_BETA || te.flag == TT_ALPHA)
                conf += 1;
            ctx.ttConfidence = (uint8_t)clampi(conf, 0, 3);
        }

        int nonPawn = 0;
        int major = 0;
        for (int sq = 0; sq < 64; sq++) {
            Piece p = pos.board[sq];
            if (p == NO_PIECE)
                continue;
            PieceType pt = type_of(p);
            if (pt == KING || pt == PAWN)
                continue;
            nonPawn++;
            if (pt == ROOK || pt == QUEEN)
                major++;
        }
        if (major == 0 && nonPawn <= 4)
            ctx.endgameRisk = 2;
        else if (nonPawn <= 6)
            ctx.endgameRisk = 1;
        else
            ctx.endgameRisk = 0;

        return ctx;
    }

    // Move ordering: TT move, captures (SEE), killers, history, and heuristics.
    inline int move_score(const Position& pos, Move m, Move ttMove, int ply, int prevFrom, int prevTo) {
        ply = std::min(ply, 127);

        if (m == ttMove)
            return 1000000000;

        int from = from_sq(m), to = to_sq(m);
        Piece mover = pos.board[from];

        int sc = 0;

        if (promo_of(m))
            sc += 90000000;
        if (flags_of(m) & MF_CASTLE)
            sc += 30000000;

        if (is_capture(pos, m)) {
            sc += 50000000;

            Piece victim = pos.board[to];
            if (flags_of(m) & MF_EP)
                victim = make_piece(flip_color(pos.side), PAWN);

            int s = see_quick(pos, m);

            if (promo_of(m) || s < -250)
                s = see_full(pos, m);

            s = clampi(s, -500, 500);
            sc += s * 8000;
            sc += mvv_lva(victim, mover) * 200;
            return sc;
        }

        if (m == killer[0][ply])
            sc += 20000000;
        else if (m == killer[1][ply])
            sc += 15000000;

        const int ci = color_index(pos.side);

        sc += history[ci][from][to] / 2;

        if ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u) {
            if (m == countermove[prevFrom][prevTo])
                sc += 18000000;
            sc += contHist[ci][prevFrom][prevTo][from][to] / 4;
        }

        if (type_of(mover) == BISHOP)
            sc += 2000;
        if (type_of(mover) == KNIGHT)
            sc += 1000;

        if (type_of(mover) == KING && !(flags_of(m) & MF_CASTLE)) {
            if (ply < 12)
                sc -= 8000000;
            else
                sc -= 800000;
        }

        if (ply < 4 && type_of(mover) == PAWN && promo_of(m) == 0) {
            if (from == E2 && to == E4)
                sc += 12000;
            if (from == D2 && to == D4)
                sc += 12000;
            if (from == E7 && to == E5)
                sc += 12000;
            if (from == D7 && to == D5)
                sc += 12000;

            if (from == C2 && to == C4)
                sc += 7000;
            if (from == C7 && to == C5)
                sc += 7000;
        }

        return sc;
    }

    // Quiescence search: captures/promotions and limited checks.
    int qsearch(Position& pos, int alpha, int beta, int ply, int lastTo, bool lastWasCap) {
        add_node();
        selDepth = std::max(selDepth, ply);

        const Color us = pos.side;
        const bool inCheck = attacks::in_check(pos, us);

        if (ply >= 64)
            return eval::evaluate(pos);

        int stand = -INF;
        if (!inCheck) {
            stand = eval::evaluate(pos);
            if (stand >= beta)
                return beta;
            if (stand > alpha)
                alpha = stand;
        }

        auto& moves = plyMoves[ply];
        movegen::generate_pseudo_legal(pos, moves);

        auto& list = plyQList[ply];
        list.clear();
        list.reserve(moves.size());

        static const int PVV[7] = {0, 100, 320, 330, 500, 900, 0};

        constexpr int QUIET_CHECK_MAX_PLY = 2;
        constexpr int DELTA_MARGIN = 140;
        constexpr int SEE_CUT = -120;
        constexpr int SEE_FULL_TRIGGER = -240;

        const bool shallow = (ply <= 1);

        for (Move m : moves) {
            if (flags_of(m) & MF_CASTLE)
                continue;

            const bool promo = (promo_of(m) != 0);
            const bool cap = is_capture(pos, m) || (flags_of(m) & MF_EP);

            const bool quietCandidate = (!inCheck && !cap && !promo && ply < QUIET_CHECK_MAX_PLY);
            if (!inCheck && !(cap || promo || quietCandidate))
                continue;

            int gain = 0;
            Piece victim = NO_PIECE;

            if (cap) {
                int to = to_sq(m);
                if (flags_of(m) & MF_EP)
                    gain += 100;
                else {
                    victim = pos.board[to];
                    gain += (victim == NO_PIECE ? 0 : PVV[type_of(victim)]);
                }
            }
            if (promo) {
                int pr = promo_of(m);
                int newv = (pr == 1 ? 320 : pr == 2 ? 330 : pr == 3 ? 500 : 900);
                gain += (newv - 100);
            }

            if (!inCheck && (cap || promo) && !shallow) {
                if (!promo && stand + gain + DELTA_MARGIN <= alpha)
                    continue;

                if (!promo) {
                    int sQ = see_quick(pos, m);

                    if (sQ <= SEE_FULL_TRIGGER) {
                        bool bigVictim = false;
                        if (flags_of(m) & MF_EP)
                            bigVictim = false;
                        else if (victim != NO_PIECE && type_of(victim) >= ROOK)
                            bigVictim = true;

                        if (bigVictim) {
                            int sF = see_full(pos, m);
                            if (sF < SEE_CUT)
                                continue;
                        } else {
                            if (sQ < SEE_CUT)
                                continue;
                        }
                    } else if (sQ < SEE_CUT) {
                        continue;
                    }
                }
            }

            if (quietCandidate && !shallow) {
                if (stand + 40 < alpha)
                    continue;
            }

            int key = 0;
            if (promo)
                key += 400000;
            if (cap)
                key += 80000;
            key += gain * 300;

            if (lastWasCap && cap && lastTo >= 0 && to_sq(m) == lastTo) {
                key += 220000;
            }

            list.push_back(QNode{m, key, cap, promo, false});
        }

        if (list.empty()) {
            if (inCheck)
                return -MATE + ply;
            return alpha;
        }

        std::sort(list.begin(), list.end(), [](const QNode& a, const QNode& b) { return a.key > b.key; });

        for (QNode& qn : list) {
            Move m = qn.m;

            Undo u = pos.do_move(m);

            if (attacks::in_check(pos, us)) {
                pos.undo_move(m, u);
                continue;
            }

            const bool givesCheck = attacks::in_check(pos, pos.side);

            if (!inCheck && !(qn.cap || qn.promo || givesCheck)) {
                pos.undo_move(m, u);
                continue;
            }

            const int nextLastTo = to_sq(m);
            const bool nextLastWasCap = qn.cap;

            int score = -qsearch(pos, -beta, -alpha, ply + 1, nextLastTo, nextLastWasCap);

            pos.undo_move(m, u);

            if (score >= beta)
                return beta;
            if (score > alpha)
                alpha = score;
        }

        return alpha;
    }

    // Null move pruning; must keep zobKey consistent.
    struct NullMoveUndo {
        int ep;
        Color side;
        uint64_t key;
    };

    inline void do_null_move(Position& pos, NullMoveUndo& u) {
        u.ep = pos.epSquare;
        u.side = pos.side;
        u.key = pos.zobKey;

        uint64_t k = pos.zobKey;

        if (pos.epSquare != -1)
            k ^= g_zob.epKey[file_of(pos.epSquare) & 7];

        k ^= g_zob.sideKey;

        pos.epSquare = -1;
        pos.side = (pos.side == WHITE) ? BLACK : WHITE;
        pos.zobKey = k;
    }

    inline void undo_null_move(Position& pos, const NullMoveUndo& u) {
        pos.epSquare = u.ep;
        pos.side = u.side;
        pos.zobKey = u.key;
    }

    inline bool has_non_pawn_material(const Position& pos, Color c) {
        for (int sq = 0; sq < 64; sq++) {
            Piece p = pos.board[sq];
            if (p == NO_PIECE)
                continue;

            if (c == WHITE) {
                int v = (int)p;
                if (v >= 1 && v <= 6) {
                    PieceType pt = type_of(p);
                    if (pt != PAWN && pt != KING)
                        return true;
                }
            } else {
                int v = (int)p;
                if (v >= 9 && v <= 14) {
                    PieceType pt = type_of(p);
                    if (pt != PAWN && pt != KING)
                        return true;
                }
            }
        }
        return false;
    }

    static inline bool is_legal_move_here(Position& pos, Move m) {
        if (!move_sane_basic(pos, m))
            return false;

        if (flags_of(m) & MF_CASTLE) {
            if (!movegen::legal_castle_path_ok(pos, m))
                return false;
        }

        const Color us = pos.side;
        Undo u = pos.do_move(m);
        const bool ok = !attacks::in_check(pos, us);
        pos.undo_move(m, u);
        return ok;
    }

    // Follow TT best moves to build a PV line, stopping on repetition or invalid moves.
    inline void follow_tt_pv(Position& pos, int maxLen, PVLine& out) {
        out.len = 0;
        if (maxLen <= 0)
            return;

        Undo undos[128];
        Move um[128];
        int ucnt = 0;

        uint64_t seen[128];
        int seenN = 0;

        auto seen_before = [&](uint64_t k) {
            for (int i = 0; i < seenN; i++)
                if (seen[i] == k)
                    return true;
            return false;
        };

        Move prev = 0;

        for (int i = 0; i < maxLen && out.len < 128; i++) {
            const uint64_t k = pos.zobKey;

            if (seen_before(k))
                break;
            if (seenN < 128)
                seen[seenN++] = k;

            TTEntry te{};
            if (!stt->probe_copy(k, te))
                break;

            Move m = te.best;
            if (!m)
                break;

            if (!is_legal_move_here(pos, m))
                break;

            if (prev && from_sq(m) == to_sq(prev) && to_sq(m) == from_sq(prev) && promo_of(m) == 0 &&
                promo_of(prev) == 0) {
                break;
            }

            um[ucnt] = m;
            undos[ucnt] = pos.do_move(m);
            ucnt++;

            if (attacks::in_check(pos, flip_color(pos.side))) {
                pos.undo_move(m, undos[--ucnt]);
                break;
            }

            out.m[out.len++] = m;
            prev = m;

            if (stop_or_time_up(false)) [[unlikely]]
                break;
        }

        for (int i = ucnt - 1; i >= 0; i--)
            pos.undo_move(um[i], undos[i]);
    }

    // Rebuild PV from root position and keep only legal prefix.
    inline PVLine sanitize_pv_from_root(const Position& root, const PVLine& raw, int maxLen = 128) {
        PVLine clean;
        clean.len = 0;
        if (raw.len <= 0 || maxLen <= 0)
            return clean;

        Position cur = root;
        const int lim = std::min({raw.len, maxLen, 128});

        for (int i = 0; i < lim; i++) {
            Move m = raw.m[i];
            if (!m)
                break;
            if (!is_legal_move_here(cur, m))
                break;

            clean.m[clean.len++] = m;
            Undo u = cur.do_move(m);
            (void)u;
        }

        return clean;
    }

    // =====================================
    // negamax
    // =====================================
    int negamax(Position& pos, int depth, int alpha, int beta, int ply, int prevFrom, int prevTo, int lastTo,
                bool lastWasCap, PVLine& pv) {
        pv.len = 0;
        const bool pvNode = (beta - alpha > 1);

        if (stop_or_time_up(false)) [[unlikely]]
            return alpha;

        add_node();
        selDepth = std::max(selDepth, ply);

        if (ply >= 128)
            return eval::evaluate(pos);

        const Color us = pos.side;
        const bool inCheck = attacks::in_check(pos, us);

        alpha = std::max(alpha, -MATE + ply);
        beta = std::min(beta, MATE - ply - 1);
        if (alpha >= beta)
            return alpha;

        const uint64_t key = pos.zobKey;

        if (ply > 0) {
            for (int i = keyPly - 2; i >= 0; i -= 2) {
                if (keyStack[i] == key)
                    return 0;
            }
        }

        keyStack[keyPly++] = key;
        struct KeyPop {
            int& p;
            KeyPop(int& x) : p(x) {}
            ~KeyPop() { p--; }
        } _kp(keyPly);

        TTEntry te{};
        if (collect_stats())
            ss.ttProbe++;
        bool ttHit = stt->probe_copy(key, te);
        if (collect_stats()) {
            if (ttHit)
                ss.ttHit++;
        }

        Move ttMove = 0;
        if (ttHit) {
            ttMove = te.best;

            if (te.depth >= depth) {
                int ttScore = from_tt_score((int)te.score, ply);
                const bool allowTTCut = (!pvNode || !g_params.ttPvConservative || te.flag == TT_EXACT);

                if (te.flag == TT_EXACT && allowTTCut) {
                    if (collect_stats())
                        ss.ttCut++;
                    if (ttMove && is_legal_move_here(pos, ttMove)) {
                        pv.m[0] = ttMove;
                        pv.len = 1;
                    }
                    return ttScore;
                }

                if (te.flag == TT_ALPHA && allowTTCut && ttScore <= alpha) {
                    if (collect_stats())
                        ss.ttCut++;
                    if (pv.len == 0 && ttMove && is_legal_move_here(pos, ttMove)) {
                        pv.m[0] = ttMove;
                        pv.len = 1;
                    }
                    return alpha;
                }

                if (te.flag == TT_BETA && allowTTCut && ttScore >= beta) {
                    if (collect_stats())
                        ss.ttCut++;
                    if (pv.len == 0 && ttMove && is_legal_move_here(pos, ttMove)) {
                        pv.m[0] = ttMove;
                        pv.len = 1;
                    }
                    return beta;
                }
            }
        }

        if (inCheck)
            depth++;

        if (depth <= 0)
            return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);

        int staticEval = inCheck ? -INF : eval::evaluate(pos);
        if (!inCheck)
            staticEvalStack[ply] = staticEval;
        else if (ply > 0)
            staticEvalStack[ply] = staticEvalStack[ply - 1];
        else
            staticEvalStack[ply] = staticEval;

        bool improving = false;
        if (!inCheck && ply >= 2 && staticEvalStack[ply - 2] > -INF / 2)
            improving = staticEval > staticEvalStack[ply - 2];

        if (g_params.enableRazoring && !inCheck && ply > 0 && depth <= g_params.razorDepthMax) {
            const int razorMargin = razor_margin(depth, improving);
            if (staticEval + razorMargin <= alpha) {
                ps.razorPrune++;
                return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);
            }
        }

        if (g_params.enableRfp && !inCheck && depth <= g_params.rfpDepthMax && ply > 0) {
            const int rfpMargin = rfp_margin(depth, improving);
            if (staticEval - rfpMargin >= beta) {
                ps.rfpPrune++;
                return beta;
            }
        }

        if (g_params.enableIIR && !inCheck && ply > 0 && !pvNode && !ttHit && depth >= g_params.iirMinDepth) {
            depth = std::max(1, depth - g_params.iirReduce);
            ps.iirApplied++;
        }

        int nTypeIdx = 2;
        if (collect_stats()) {
            NodeContext ctx = make_node_context(pos, depth, alpha, beta, ply, inCheck, staticEval, improving, ttHit, te);
            if (ctx.nodeType == NodeContext::PV)
                ss.nodePv++;
            else if (ctx.nodeType == NodeContext::CUT)
                ss.nodeCut++;
            else
                ss.nodeAll++;
            nTypeIdx = node_type_index(ctx.nodeType);
            ss.nodeByType[nTypeIdx]++;
        }

        if (g_params.enableNullMove && !inCheck && depth >= g_params.nullMinDepth && ply > 0) {
            if (has_non_pawn_material(pos, us)) {
                int R = g_params.nullBase + depth / g_params.nullDepthDiv;
                R = std::min(R, depth - 1);

                if (beta < MATE - g_params.nullMateGuard && alpha > -MATE + g_params.nullMateGuard) {
                    if (collect_stats())
                        ss.nullTried++;
                    NullMoveUndo nu;
                    do_null_move(pos, nu);

                    PVLine npv;
                    int score = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1, -1, -1, -1, false, npv);

                    undo_null_move(pos, nu);

                    if (stop_or_time_up(false)) [[unlikely]]
                        return alpha;
                    if (score >= beta) {
                        if (collect_stats())
                            ss.nullCut++;
                        return beta;
                    }
                }
            }
        }

        auto& moves = plyMoves[ply];
        auto& scores = plyScores[ply];

        movegen::generate_pseudo_legal(pos, moves);

        if (moves.empty()) {
            if (inCheck)
                return -MATE + ply;
            return 0;
        }

        scores.resize(moves.size());
        for (int i = 0; i < (int)moves.size(); i++)
            scores[i] = move_score(pos, moves[i], ttMove, ply, prevFrom, prevTo);

        auto& order = plyOrder[ply];
        order.resize(moves.size());
        for (int i = 0; i < (int)moves.size(); i++)
            order[i] = i;

        int K = std::min<int>(g_params.nodeOrderK, (int)order.size());

        for (int i = 0; i < K; i++) {
            int bi = i;
            int bs = scores[order[i]];
            for (int j = i + 1; j < (int)order.size(); j++) {
                int sj = scores[order[j]];
                if (sj > bs) {
                    bs = sj;
                    bi = j;
                }
            }
            if (bi != i)
                std::swap(order[i], order[bi]);
        }

        int bestScore = -INF;
        Move bestMove = 0;
        const int origAlpha = alpha;

        PVLine bestPV;
        bestPV.len = 0;

        int legalMovesSearched = 0;
        int quietMovesSearched = 0;
        if (collect_stats())
            ss.moveLoopNodes++;

        bool ttAvail = false;
        bool ttFirstAccounted = false;
        if (collect_stats() && ttHit && ttMove) {
            ttAvail = true;
            ss.ttMoveAvail++;
        }

        for (int kk = 0; kk < (int)order.size(); kk++) {
            if (stop_or_time_up(false)) [[unlikely]]
                return alpha;

            Move m = moves[order[kk]];
            const int curFrom = from_sq(m);
            const int curTo = to_sq(m);

            const bool isCap = is_capture(pos, m) || (flags_of(m) & MF_EP);
            const bool isPromo = (promo_of(m) != 0);
            const bool isQuiet = (!isCap && !isPromo);

            if (g_params.enableQuietFutility && !inCheck && isQuiet && ply > 0 && depth <= g_params.quietFutilityDepthMax &&
                m != ttMove) {
                const int futMargin = quiet_futility_margin(depth, improving);
                if (staticEval + futMargin <= alpha) {
                    ps.quietFutility++;
                    if (collect_stats())
                        ss.futilitySkip++;
                    continue;
                }
            }

            if (g_params.enableQuietLimit && !inCheck && isQuiet && ply > 0 && depth <= g_params.quietLimitDepthMax &&
                m != ttMove) {
                const int limit = quiet_limit_for_depth(depth);
                if (quietMovesSearched >= limit) {
                    ps.quietLimit++;
                    if (collect_stats())
                        ss.lmpSkip++;
                    continue;
                }
            }

            if (g_params.enableCapSeePrune && !inCheck && isCap && !isPromo && ply > 0 && depth <= g_params.capSeeDepthMax &&
                m != ttMove) {
                int sQ = see_quick(pos, m);
                if (sQ < g_params.capSeeQuickFullTrigger) {
                    int sF = see_full(pos, m);
                    if (sF < g_params.capSeeFullCut) {
                        ps.capSeePrune++;
                        continue;
                    }
                } else if (sQ < g_params.capSeeQuickCut) {
                    ps.capSeePrune++;
                    continue;
                }
            }

            Undo u = pos.do_move(m);

            if (attacks::in_check(pos, us)) {
                pos.undo_move(m, u);
                continue;
            }

            legalMovesSearched++;
            if (collect_stats())
                ss.totalLegalTried++;
            if (collect_stats())
                ss.legalByType[nTypeIdx]++;
            if (collect_stats() && ttAvail && !ttFirstAccounted && legalMovesSearched == 1) {
                if (m == ttMove)
                    ss.ttMoveFirst++;
                ttFirstAccounted = true;
            }
            if (isQuiet)
                quietMovesSearched++;

            const int nextLastTo = to_sq(m);
            const bool nextLastWasCap = isCap;

            int score;
            PVLine childPV;

            if (legalMovesSearched == 1) {
                if (collect_stats())
                    ss.firstMoveTried++;
                score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                 childPV);
            } else {
                int reduction = 0;
                if (ply > 0) {
                    reduction = compute_lmr_reduction(depth, legalMovesSearched, inCheck, isQuiet, improving, pvNode, us,
                                                      curFrom, curTo);
                    if (reduction > 0) {
                        ps.lmrApplied++;
                        if (collect_stats()) {
                            ss.lmrTried++;
                            bool isK = (m == killer[0][std::min(ply, 127)] || m == killer[1][std::min(ply, 127)]);
                            bool isC = ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u &&
                                        m == countermove[prevFrom][prevTo]);
                            const bool recapture = (lastWasCap && lastTo >= 0 && curTo == lastTo);
                            const bool givesCheck = attacks::in_check(pos, pos.side);
                            const int qh = quiet_bucket_score(us, curFrom, curTo, prevFrom, prevTo);
                            ss.lmrReducedByBucket[lmr_bucket_refined(isK, isC, recapture, givesCheck, qh)]++;
                        }
                    }
                }

                int rd = depth - 1 - reduction;
                if (rd < 0)
                    rd = 0;

                score =
                    -negamax(pos, rd, -alpha - 1, -alpha, ply + 1, curFrom, curTo, nextLastTo, nextLastWasCap, childPV);

                if (score > alpha && reduction > 0 && rd != depth - 1) {
                    if (collect_stats()) {
                        ss.lmrResearched++;
                        bool isK = (m == killer[0][std::min(ply, 127)] || m == killer[1][std::min(ply, 127)]);
                        bool isC = ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u &&
                                    m == countermove[prevFrom][prevTo]);
                        const bool recapture = (lastWasCap && lastTo >= 0 && curTo == lastTo);
                        const bool givesCheck = attacks::in_check(pos, pos.side);
                        const int qh = quiet_bucket_score(us, curFrom, curTo, prevFrom, prevTo);
                        ss.lmrResearchedByBucket[lmr_bucket_refined(isK, isC, recapture, givesCheck, qh)]++;
                    }
                    PVLine childPV2;
                    score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1, curFrom, curTo, nextLastTo,
                                     nextLastWasCap, childPV2);
                    childPV = childPV2;
                }

                if (score > alpha && score < beta) {
                    PVLine childPV3;
                    score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV3);
                    childPV = childPV3;
                }
            }

            pos.undo_move(m, u);

            if (stop_or_time_up(false)) [[unlikely]]
                return alpha;

            if (score > bestScore) {
                bestScore = score;
                bestMove = m;

                bestPV.m[0] = m;
                bestPV.len = std::min(127, childPV.len + 1);
                for (int k = 0; k < childPV.len && k + 1 < 128; k++) {
                    bestPV.m[k + 1] = childPV.m[k];
                }
            }

            if (score > alpha)
                alpha = score;
            else if (isQuiet && ply < 128)
                update_quiet_history(us, prevFrom, prevTo, curFrom, curTo, depth, false);

            if (alpha >= beta) {
                ps.betaCutoff++;
                if (collect_stats()) {
                    if (legalMovesSearched == 1)
                        ss.firstMoveFailHigh++;
                }
                if (isQuiet && ply < 128) {
                    if (killer[0][ply] != m) {
                        killer[1][ply] = killer[0][ply];
                        killer[0][ply] = m;
                    }

                    update_quiet_history(us, prevFrom, prevTo, curFrom, curTo, depth, true);

                    if ((unsigned)prevFrom < 64u && (unsigned)prevTo < 64u) {
                        countermove[prevFrom][prevTo] = m;
                    }
                }
                break;
            }
        }

        if (legalMovesSearched == 0)
            return inCheck ? (-MATE + ply) : 0;

        pv = bestPV;

        {
            TTFlag flag = TT_EXACT;
            if (bestScore <= origAlpha)
                flag = TT_ALPHA;
            else if (bestScore >= beta)
                flag = TT_BETA;

            int store = clampi(bestScore, -INF, INF);
            store = to_tt_score(store, ply);

            stt->store(key, bestMove, (int16_t)store, (int16_t)depth, (uint8_t)flag);
        }
        if (collect_stats()) {
            if (ttHit && bestMove == ttMove && bestMove)
                ss.ttBest++;
        }

        return bestScore;
    }

    // Iterative deepening with aspiration windows and root move ordering.
    Result think(Position& pos, const Limits& lim, bool emitInfo) {
        keyPly = 0;
        keyStack[keyPly++] = pos.zobKey;
        for (int i = 0; i < 256; i++)
            staticEvalStack[i] = -INF;

        Result res{};
        nodes = 0;
        nodes_batch = 0;
        time_check_tick = 0;
        selDepth = 0;
        ps.clear();
        ss.clear();

        const int maxDepth = (lim.depth > 0 ? lim.depth : 64);
        const int64_t startT = now_ms();
        int lastFlushMs = 0;
        int lastInfoMs = -1000000;

        std::vector<Move> rootMoves;
        rootMoves.reserve(256);
        movegen::generate_legal(pos, rootMoves);

        if (rootMoves.empty()) {
            flush_nodes_batch();
            res.bestMove = 0;
            res.ponderMove = 0;
            res.score = 0;
            res.nodes = nodes;
            return res;
        }

        Move bestMove = rootMoves[0];
        int bestScore = -INF;

        constexpr int ASP_START = 35;
        constexpr int PV_MAX = 128;
        // Combine global nodes and local batch for consistent info output.
        auto now_time_nodes_nps = [&]() {
            int t = (int)(now_ms() - startT);
            if (t < 1)
                t = 1;

            uint64_t nodesAll = g_nodes_total.load(std::memory_order_relaxed) + nodes_batch;

            uint64_t npsAll = (nodesAll * 1000ULL) / (uint64_t)t;

            return std::tuple<int, uint64_t, uint64_t>(t, npsAll, nodesAll);
        };

        PVLine rootPV;
        PVLine rootPVLegal;

        auto classify_root_source = [&](Move m, Move ttM) {
            if (m && ttM && m == ttM)
                return 0; // tt
            const bool cap = is_capture(pos, m) || (flags_of(m) & MF_EP) || promo_of(m);
            if (cap)
                return 1; // capture
            if (m == killer[0][0] || m == killer[1][0])
                return 2; // killer
            // Root has no natural predecessor, so countermove at root is generally unavailable.
            return 4; // quiet
        };

        // Root search with PVS and late-move reductions (root-specific heuristics).
        auto root_search = [&](int d, int alpha, int beta, Move& outBestMove, int& outBestScore, PVLine& outPV,
                               bool& outOk) {
            outOk = true;

            int curAlpha = alpha;
            int curBeta = beta;

            Move iterBestMove = 0;
            int iterBestScore = -INF;
            PVLine iterPV;
            iterPV.len = 0;
            int iterBestIndex = -1;
            bool firstMoveCut = false;

            int rootLegalsSearched = 0;

            for (int i = 0; i < (int)rootMoves.size(); i++) {
                if (stop_or_time_up(true)) [[unlikely]] {
                    outOk = false;
                    break;
                }

                Move m = rootMoves[i];

                const bool isCap = is_capture(pos, m) || (flags_of(m) & MF_EP);
                const bool isPromo = (promo_of(m) != 0);

                Undo u = pos.do_move(m);

                rootLegalsSearched++;

                bool givesCheck = false;
                if (d >= 6 && i >= 4) {
                    givesCheck = attacks::in_check(pos, pos.side);
                }

                const int nextLastTo = to_sq(m);
                const bool nextLastWasCap = isCap;

                int score = -INF;

                int r = 0;
                if (!isCap && !isPromo && !givesCheck && d >= 6 && i >= 4) {
                    r = 1;
                    if (d >= 10 && i >= 10)
                        r = 2;
                    r = std::min(r, d - 2);
                }

                PVLine childPV;

                const int curFrom = from_sq(m);
                const int curTo = to_sq(m);

                if (rootLegalsSearched == 1) {
                    score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV);
                } else {
                    if (collect_stats())
                        ss.rootNonFirstTried++;
                    int rd = (d - 1) - r;
                    if (rd < 0)
                        rd = 0;

                    score = -negamax(pos, rd, -curAlpha - 1, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV);

                    if (score > curAlpha && score < curBeta) {
                        if (collect_stats()) {
                            ss.rootPvsReSearch++;
                            if (r > 0)
                                ss.rootLmrReSearch++;
                        }
                        PVLine childPV2;
                        score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                         childPV2);
                        childPV = childPV2;
                    }
                }

                pos.undo_move(m, u);

                if (stop_or_time_up(true)) [[unlikely]] {
                    outOk = false;
                    break;
                }

                if (score > iterBestScore) {
                    iterBestScore = score;
                    iterBestMove = m;
                    iterBestIndex = i;

                    iterPV.m[0] = m;
                    iterPV.len = std::min(127, childPV.len + 1);
                    for (int k = 0; k < childPV.len && k + 1 < 128; k++) {
                        iterPV.m[k + 1] = childPV.m[k];
                    }
                }

                if (score > curAlpha)
                    curAlpha = score;
                if (curAlpha >= curBeta) {
                    if (i == 0)
                        firstMoveCut = true;
                    break;
                }
            }

            if (rootLegalsSearched == 0)
                outOk = false;

            if (collect_stats() && rootLegalsSearched > 0) {
                ss.rootIters++;
                if (iterBestIndex == 0 || firstMoveCut)
                    ss.rootFirstBestOrCut++;
            }

            outBestMove = iterBestMove;
            outBestScore = iterBestScore;
            outPV = iterPV;
        };

        Move prevIterBestMove = 0;
        int prevIterScore = 0;
        bool prevHadNull = false;
        bool prevHadRfp = false;
        bool prevHadRazor = false;

        for (int d = 1; d <= maxDepth; d++) {
            if (stop_or_time_up(true)) [[unlikely]]
                break;

            if (bestMove) {
                auto it = std::find(rootMoves.begin(), rootMoves.end(), bestMove);
                if (it != rootMoves.end())
                    std::swap(rootMoves[0], *it);
            }

            std::vector<int> order(rootMoves.size());
            for (int i = 0; i < (int)rootMoves.size(); i++)
                order[i] = move_score(pos, rootMoves[i], bestMove, 0, -1, -1);

            const int K = std::min<int>(g_params.rootOrderK, (int)rootMoves.size());
            for (int i = 0; i < K; i++) {
                int bi = i, bs = order[i];
                for (int j = i + 1; j < (int)rootMoves.size(); j++) {
                    if (order[j] > bs) {
                        bs = order[j];
                        bi = j;
                    }
                }
                if (bi != i) {
                    std::swap(rootMoves[i], rootMoves[bi]);
                    std::swap(order[i], order[bi]);
                }
            }

            const bool useAsp = (d > 5 && bestScore > -INF / 2 && bestScore < INF / 2);
            int alpha = useAsp ? (bestScore - ASP_START) : -INF;
            int beta = useAsp ? (bestScore + ASP_START) : INF;

            Move localBestMove = bestMove;
            int localBestScore = bestScore;
            PVLine localPV;
            localPV.len = 0;
            bool ok = false;
            Move rootTTMove = 0;
            {
                TTEntry rte{};
                if (stt->probe_copy(pos.zobKey, rte) && rte.best && is_legal_move_here(pos, rte.best))
                    rootTTMove = rte.best;
            }

            const uint64_t pRazor0 = ps.razorPrune;
            const uint64_t pRfp0 = ps.rfpPrune;
            const uint64_t pNull0 = ss.nullTried;

            root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
            if (!ok)
                break;

            if (useAsp && (localBestScore <= alpha || localBestScore >= beta)) {
                if (collect_stats())
                    ss.aspFail++;
                alpha = -INF;
                beta = INF;
                root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
                if (!ok)
                    break;
            }

            if (collect_stats()) {
                ss.rootBestSrc[classify_root_source(localBestMove, rootTTMove)]++;
                const bool hadRazor = (ps.razorPrune > pRazor0);
                const bool hadRfp = (ps.rfpPrune > pRfp0);
                const bool hadNull = (ss.nullTried > pNull0);
                if (prevIterBestMove && localBestMove && localBestMove != prevIterBestMove &&
                    std::abs(localBestScore - prevIterScore) >= 120) {
                    if (prevHadNull)
                        ss.proxyReversalAfterNull++;
                    if (prevHadRfp)
                        ss.proxyReversalAfterRfp++;
                    if (prevHadRazor)
                        ss.proxyReversalAfterRazor++;
                }
                prevHadNull = hadNull;
                prevHadRfp = hadRfp;
                prevHadRazor = hadRazor;
                prevIterBestMove = localBestMove;
                prevIterScore = localBestScore;
            }

            bestMove = localBestMove;
            bestScore = localBestScore;
            rootPV = localPV;

            if (emitInfo) {
                auto [t, nps, nodesAll] = now_time_nodes_nps();
                const bool shouldEmit = (d <= 6) || (t - lastInfoMs >= 90);
                if (shouldEmit) {
                    rootPVLegal = sanitize_pv_from_root(pos, rootPV, PV_MAX);
                    int hashfull = stt->hashfull_permille();
                    int sd = std::max(1, selDepth);

                    std::cout << "info depth " << d << " seldepth " << sd << " multipv 1 ";
                    print_score_uci(bestScore);
                    std::cout << " nodes " << nodesAll << " nps " << nps << " hashfull " << hashfull << " tbhits 0"
                              << " time " << t << " pv ";

                    int outN = std::min(PV_MAX, rootPVLegal.len);
                    for (int i = 0; i < outN; i++) {
                        Move pm = rootPVLegal.m[i];
                        if (!pm)
                            break;
                        std::cout << move_to_uci(pm) << " ";
                    }
                    std::cout << "\n";

                    int curMs = t;
                    if (curMs - lastFlushMs >= 50) {
                        std::cout.flush();
                        lastFlushMs = curMs;
                    }
                    lastInfoMs = t;
                }
            }
        }

        flush_nodes_batch();

        res.bestMove = bestMove;
        res.score = bestScore;
        res.nodes = nodes;

        rootPVLegal = sanitize_pv_from_root(pos, rootPV, PV_MAX);
        res.ponderMove = (rootPVLegal.len >= 2 ? rootPVLegal.m[1] : 0);

        if (emitInfo) {
            std::cout << "info string prune razor=" << ps.razorPrune << " rfp=" << ps.rfpPrune
                      << " qfut=" << ps.quietFutility << " qlim=" << ps.quietLimit
                      << " csee=" << ps.capSeePrune << " iir=" << ps.iirApplied << " lmr=" << ps.lmrApplied
                      << " bcut=" << ps.betaCutoff << "\n";
            if (collect_stats()) {
                uint64_t rootDen = ss.rootIters ? ss.rootIters : 1;
                uint64_t ttDen = ss.ttProbe ? ss.ttProbe : 1;
                uint64_t ttMoveDen = ss.ttMoveAvail ? ss.ttMoveAvail : 1;
                uint64_t lmrDen = ss.lmrTried ? ss.lmrTried : 1;
                uint64_t rootReDen = ss.rootNonFirstTried ? ss.rootNonFirstTried : 1;

                auto pct = [](uint64_t num, uint64_t den) { return (1000ULL * num) / (den ? den : 1); };
                uint64_t avgPv = (ss.nodeByType[0] ? (1000ULL * ss.legalByType[0] / ss.nodeByType[0]) : 0);
                uint64_t avgCut = (ss.nodeByType[1] ? (1000ULL * ss.legalByType[1] / ss.nodeByType[1]) : 0);
                uint64_t avgAll = (ss.nodeByType[2] ? (1000ULL * ss.legalByType[2] / ss.nodeByType[2]) : 0);

                std::cout << "info string stats_root fh1=" << pct(ss.rootFirstBestOrCut, rootDen)
                          << " re=" << pct(ss.rootPvsReSearch, rootReDen)
                          << " src_tt=" << pct(ss.rootBestSrc[0], rootDen) << " src_cap=" << pct(ss.rootBestSrc[1], rootDen)
                          << " src_k=" << pct(ss.rootBestSrc[2], rootDen) << " src_c=" << pct(ss.rootBestSrc[3], rootDen)
                          << " src_q=" << pct(ss.rootBestSrc[4], rootDen) << " asp=" << ss.aspFail << "\n";

                std::cout << "info string stats_node pv=" << ss.nodeByType[0] << " cut=" << ss.nodeByType[1]
                          << " all=" << ss.nodeByType[2] << " avgm_pv=" << avgPv << " avgm_cut=" << avgCut
                          << " avgm_all=" << avgAll << " tt_hit=" << pct(ss.ttHit, ttDen)
                          << " tt_cut=" << pct(ss.ttCut, ttDen) << " ttm_first=" << pct(ss.ttMoveFirst, ttMoveDen)
                          << "\n";

                std::cout << "info string stats_lmr red=" << ss.lmrTried << " re=" << pct(ss.lmrResearched, lmrDen)
                          << " rk=" << ss.lmrReducedByBucket[0] << " rc=" << ss.lmrReducedByBucket[1]
                          << " rh=" << ss.lmrReducedByBucket[2] << " rl=" << ss.lmrReducedByBucket[3]
                          << " rek=" << ss.lmrResearchedByBucket[0] << " rec=" << ss.lmrResearchedByBucket[1]
                          << " reh=" << ss.lmrResearchedByBucket[2] << " rel=" << ss.lmrResearchedByBucket[3] << "\n";

                std::cout << "info string stats_prune null_t=" << ss.nullTried << " null_fh=" << ss.nullCut
                          << " null_vf=" << ss.nullVerifyFail << " raz=" << ps.razorPrune << " rfp=" << ps.rfpPrune
                          << " rev_null=" << ss.proxyReversalAfterNull << " rev_rfp=" << ss.proxyReversalAfterRfp
                          << " rev_raz=" << ss.proxyReversalAfterRazor << " tchk=" << ss.timeChecks << "\n";
            }
        }

        return res;
    }
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
        g_pool.push_back(g_pool_owner.back().get());
    }
}

// Public search API (single-thread or Lazy SMP).
inline Result think(Position& pos, const Limits& lim) {
    ensure_pool();

    g_nodes_total.store(0, std::memory_order_relaxed);
    start_timer(lim.movetime_ms, lim.infinite);

    int n = threads();
    if (n <= 1) {
        Result r = g_pool[0]->think(pos, lim, true);
        r.nodes = g_nodes_total.load(std::memory_order_relaxed);
        return r;
    }

    std::vector<std::thread> workers;
    workers.reserve(n - 1);

    // Worker threads search independent copies of the root position.
    for (int i = 1; i < n; i++) {
        Position pcopy = pos;
        workers.emplace_back([i, pcopy, lim]() mutable { g_pool[i]->think(pcopy, lim, false); });
    }

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










