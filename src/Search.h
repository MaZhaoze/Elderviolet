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

    // Batch node counting to reduce atomic contention.
    uint64_t nodes_batch = 0;
    static constexpr uint64_t NODE_BATCH = 4096; // power of two

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

            if (time_up()) [[unlikely]]
                break;
        }

        for (int i = ucnt - 1; i >= 0; i--)
            pos.undo_move(um[i], undos[i]);
    }

    // =====================================
    // negamax
    // =====================================
    int negamax(Position& pos, int depth, int alpha, int beta, int ply, int prevFrom, int prevTo, int lastTo,
                bool lastWasCap, PVLine& pv) {
        pv.len = 0;

        if (time_up()) [[unlikely]]
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
        bool ttHit = stt->probe_copy(key, te);

        Move ttMove = 0;
        if (ttHit) {
            ttMove = te.best;

            if (te.depth >= depth) {
                int ttScore = from_tt_score((int)te.score, ply);

                if (te.flag == TT_EXACT) {
                    if (ttMove && is_legal_move_here(pos, ttMove)) {
                        pv.m[0] = ttMove;
                        pv.len = 1;
                    }
                    return ttScore;
                }

                if (te.flag == TT_ALPHA && ttScore <= alpha) {
                    if (pv.len == 0 && ttMove && is_legal_move_here(pos, ttMove)) {
                        pv.m[0] = ttMove;
                        pv.len = 1;
                    }
                    return alpha;
                }

                if (te.flag == TT_BETA && ttScore >= beta) {
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

        if (!inCheck && ply > 0 && depth <= 2) {
            const int razorMargin = (depth == 1 ? 220 : 320);
            if (staticEval + razorMargin <= alpha) {
                return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);
            }
        }

        if (!inCheck && depth <= 3 && ply > 0) {
            const int rfpMargin = 120 + 90 * depth;
            if (staticEval - rfpMargin >= beta)
                return beta;
        }

        if (!inCheck && depth >= 3 && ply > 0) {
            if (has_non_pawn_material(pos, us)) {
                int R = 3 + depth / 6;
                R = std::min(R, depth - 1);

                if (beta < MATE - 256 && alpha > -MATE + 256) {
                    NullMoveUndo nu;
                    do_null_move(pos, nu);

                    PVLine npv;
                    int score = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1, -1, -1, -1, false, npv);

                    undo_null_move(pos, nu);

                    if (time_up()) [[unlikely]]
                        return alpha;
                    if (score >= beta)
                        return beta;
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

        constexpr int NODE_ORDER_K = 12;
        int K = std::min<int>(NODE_ORDER_K, (int)order.size());

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

        for (int kk = 0; kk < (int)order.size(); kk++) {
            if (time_up()) [[unlikely]]
                return alpha;

            Move m = moves[order[kk]];
            const int curFrom = from_sq(m);
            const int curTo = to_sq(m);

            const bool isCap = is_capture(pos, m) || (flags_of(m) & MF_EP);
            const bool isPromo = (promo_of(m) != 0);
            const bool isQuiet = (!isCap && !isPromo);

            if (!inCheck && isQuiet && ply > 0 && depth <= 2 && m != ttMove) {
                const int futMargin = (depth == 1) ? 190 : 290;
                if (staticEval + futMargin <= alpha)
                    continue;
            }

            if (!inCheck && isQuiet && ply > 0 && depth <= 2 && m != ttMove) {
                const int limit = (depth == 1) ? 5 : 8;
                if (quietMovesSearched >= limit)
                    continue;
            }

            if (!inCheck && isCap && !isPromo && ply > 0 && depth <= 4 && m != ttMove) {
                int sQ = see_quick(pos, m);
                if (sQ < -200) {
                    int sF = see_full(pos, m);
                    if (sF < -120)
                        continue;
                } else if (sQ < -120) {
                    continue;
                }
            }

            Undo u = pos.do_move(m);

            if (attacks::in_check(pos, us)) {
                pos.undo_move(m, u);
                continue;
            }

            legalMovesSearched++;
            if (isQuiet)
                quietMovesSearched++;

            const int nextLastTo = to_sq(m);
            const bool nextLastWasCap = isCap;

            int score;
            PVLine childPV;

            if (legalMovesSearched == 1) {
                score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                 childPV);
            } else {
                int reduction = 0;
                if (depth >= 3 && !inCheck && isQuiet && ply > 0) {
                    reduction = 1;
                    if (legalMovesSearched > 4)
                        reduction++;
                    if (legalMovesSearched > 10)
                        reduction++;
                    if (depth >= 7 && legalMovesSearched > 14)
                        reduction++;
                    reduction = std::min(reduction, depth - 2);

                    int ci = color_index(us);
                    int h = history[ci][curFrom][curTo] / 2;
                    if (h < 2000)
                        reduction++;
                    if (h > 60000)
                        reduction = std::max(0, reduction - 1);
                    reduction = std::min(reduction, depth - 2);
                }

                int rd = depth - 1 - reduction;
                if (rd < 0)
                    rd = 0;

                score =
                    -negamax(pos, rd, -alpha - 1, -alpha, ply + 1, curFrom, curTo, nextLastTo, nextLastWasCap, childPV);

                if (score > alpha && reduction > 0 && rd != depth - 1) {
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

            if (time_up()) [[unlikely]]
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

            if (alpha >= beta) {
                if (isQuiet && ply < 128) {
                    if (killer[0][ply] != m) {
                        killer[1][ply] = killer[0][ply];
                        killer[0][ply] = m;
                    }

                    int ci = color_index(us);

                    history[ci][curFrom][curTo] += depth * depth * 16;
                    if (history[ci][curFrom][curTo] > 300000)
                        history[ci][curFrom][curTo] = 300000;

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

        return bestScore;
    }

    // Iterative deepening with aspiration windows and root move ordering.
    Result think(Position& pos, const Limits& lim, bool emitInfo) {
        keyPly = 0;
        keyStack[keyPly++] = pos.zobKey;

        Result res{};
        nodes = 0;
        nodes_batch = 0;
        selDepth = 0;

        const int maxDepth = (lim.depth > 0 ? lim.depth : 64);
        const int64_t startT = now_ms();
        int lastFlushMs = 0;

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
        constexpr int ROOT_ORDER_K = 10;

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

            int rootLegalsSearched = 0;

            for (int i = 0; i < (int)rootMoves.size(); i++) {
                if (time_up()) [[unlikely]] {
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
                    int rd = (d - 1) - r;
                    if (rd < 0)
                        rd = 0;

                    score = -negamax(pos, rd, -curAlpha - 1, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV);

                    if (score > curAlpha && score < curBeta) {
                        PVLine childPV2;
                        score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                         childPV2);
                        childPV = childPV2;
                    }
                }

                pos.undo_move(m, u);

                if (time_up()) [[unlikely]] {
                    outOk = false;
                    break;
                }

                if (score > iterBestScore) {
                    iterBestScore = score;
                    iterBestMove = m;

                    iterPV.m[0] = m;
                    iterPV.len = std::min(127, childPV.len + 1);
                    for (int k = 0; k < childPV.len && k + 1 < 128; k++) {
                        iterPV.m[k + 1] = childPV.m[k];
                    }
                }

                if (score > curAlpha)
                    curAlpha = score;
                if (curAlpha >= curBeta)
                    break;
            }

            if (rootLegalsSearched == 0)
                outOk = false;

            outBestMove = iterBestMove;
            outBestScore = iterBestScore;
            outPV = iterPV;
        };

        for (int d = 1; d <= maxDepth; d++) {
            if (time_up()) [[unlikely]]
                break;

            if (bestMove) {
                auto it = std::find(rootMoves.begin(), rootMoves.end(), bestMove);
                if (it != rootMoves.end())
                    std::swap(rootMoves[0], *it);
            }

            std::vector<int> order(rootMoves.size());
            for (int i = 0; i < (int)rootMoves.size(); i++)
                order[i] = move_score(pos, rootMoves[i], bestMove, 0, -1, -1);

            const int K = std::min<int>(ROOT_ORDER_K, (int)rootMoves.size());
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

            root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
            if (!ok)
                break;

            if (useAsp && (localBestScore <= alpha || localBestScore >= beta)) {
                alpha = -INF;
                beta = INF;
                root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
                if (!ok)
                    break;
            }

            bestMove = localBestMove;
            bestScore = localBestScore;
            rootPV = localPV;

            if (emitInfo) {
                auto [t, nps, nodesAll] = now_time_nodes_nps();
                int hashfull = stt->hashfull_permille();
                int sd = std::max(1, selDepth);

                std::cout << "info depth " << d << " seldepth " << sd << " multipv 1 ";
                print_score_uci(bestScore);
                std::cout << " nodes " << nodesAll << " nps " << nps << " hashfull " << hashfull << " tbhits 0"
                          << " time " << t << " pv ";

                int outN = std::min(PV_MAX, rootPV.len);
                for (int i = 0; i < outN; i++) {
                    Move pm = rootPV.m[i];
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
            }
        }

        flush_nodes_batch();

        res.bestMove = bestMove;
        res.score = bestScore;
        res.nodes = nodes;

        res.ponderMove = (rootPV.len >= 2 ? rootPV.m[1] : 0);

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
