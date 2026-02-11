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

#include "types.h"
#include "Position.h"
#include "MoveGeneration.h"
#include "Attack.h"
#include "Evaluation.h"
#include "TT.h"
#include "see_full.h"

namespace search {

inline bool is_white_piece_s(Piece p) {
    int v = (int)p;
    return v >= 1 && v <= 6;
}
inline bool is_black_piece_s(Piece p) {
    int v = (int)p;
    return v >= 9 && v <= 14;
}

// =====================
// time management (旧函数保留，你现在的 Engine 侧已经重写过更好的 time split)
// =====================
static inline int compute_think_time_ms(int mytime_ms, int myinc_ms, int movestogo) {
    if (mytime_ms <= 0) return -1;

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

inline bool move_sane_basic(const Position& pos, Move m) {
    if (!m) return false;

    int from = from_sq(m), to = to_sq(m);
    if ((unsigned)from >= 64u || (unsigned)to >= 64u) return false;

    Piece pc = pos.board[from];
    if (pc == NO_PIECE) return false;

    // from must be side-to-move piece
    if (pos.side == WHITE) {
        if (!is_white_piece_s(pc)) return false;
    } else {
        if (!is_black_piece_s(pc)) return false;
    }

    // no self-capture (except EP)
    if (!(flags_of(m) & MF_EP)) {
        Piece cap = pos.board[to];
        if (cap != NO_PIECE) {
            if (pos.side == WHITE) {
                if (is_white_piece_s(cap)) return false;
            } else {
                if (is_black_piece_s(cap)) return false;
            }
        }
    }
    return true;
}

// =====================================
// SearchInfo + globals (HEADER SAFE)
// =====================================
struct SearchInfo {
    int depth = 0;
    int seldepth = 0;
    uint64_t nodes = 0;
    int time_ms = 0;
    int nps = 0;
    int hashfull = 0;   // 0..1000
};

// 这两个保持原样（selDepth 每线程各自维护）
inline int selDepth = 0;
inline std::chrono::steady_clock::time_point startTime;

// =====================================
// constants
// =====================================
static constexpr int INF  = 30000;
static constexpr int MATE = 29000;

// =====================================
// Color helpers (禁止 ~color)
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
inline std::atomic<bool>    g_stop{false};
inline std::atomic<int64_t> g_endTimeMs{0};

// 全线程合计节点数（鳕鱼味 nodes/nps）
inline std::atomic<uint64_t> g_nodes_total{0};

inline int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

inline void stop() { g_stop.store(true, std::memory_order_relaxed); }

inline void start_timer(int movetime_ms, bool infinite) {
    g_stop.store(false, std::memory_order_relaxed);
    if (infinite || movetime_ms <= 0) g_endTimeMs.store(0, std::memory_order_relaxed);
    else g_endTimeMs.store(now_ms() + (int64_t)movetime_ms, std::memory_order_relaxed);
}

inline bool time_up() {
    if (g_stop.load(std::memory_order_relaxed)) return true;
    int64_t end = g_endTimeMs.load(std::memory_order_relaxed);
    if (end == 0) return false;
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
    int  score = 0;
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
        if (score < 0) mateIn = -mateIn;
        std::cout << "score mate " << mateIn;
    } else {
        std::cout << "score cp " << score;
    }
}

// =====================================
// TT mate distance fix
// =====================================
inline int to_tt_score(int s, int ply) {
    if (s >  MATE - 256) return s + ply;
    if (s < -MATE + 256) return s - ply;
    return s;
}
inline int from_tt_score(int s, int ply) {
    if (s >  MATE - 256) return s - ply;
    if (s < -MATE + 256) return s + ply;
    return s;
}

// =====================================
// Zobrist
// =====================================
struct Zobrist {
    uint64_t psq[16][64]{};
    uint64_t sideKey = 0;
    uint64_t castleKey[16]{};
    uint64_t epKey[8]{};

    Zobrist() {
        std::mt19937_64 rng(20260126ULL);
        auto R = [&]() { return rng(); };

        for (int p = 0; p < 16; p++)
            for (int s = 0; s < 64; s++)
                psq[p][s] = R();

        sideKey = R();

        for (int i = 0; i < 16; i++) castleKey[i] = R();
        for (int i = 0; i < 8; i++)  epKey[i] = R();
    }

    inline uint64_t key(const Position& pos) const {
        uint64_t k = 0;
        for (int sq = 0; sq < 64; sq++) {
            Piece p = pos.board[sq];
            if (p == NO_PIECE) continue;

            int pi = (int)p;
            if (pi < 0 || pi >= 16) continue;

            k ^= psq[pi][sq];
        }

        if (pos.side == BLACK) k ^= sideKey;

        k ^= castleKey[pos.castlingRights & 15];

        if (pos.epSquare != -1) {
            int f = file_of(pos.epSquare);
            k ^= epKey[f & 7];
        }
        return k;
    }
};

// =====================================
// move->uci
// =====================================
inline char file_char(int f) { return char('a' + f); }
inline char rank_char(int r) { return char('1' + r); }

inline std::string move_to_uci(Move m) {
    int f = from_sq(m), t = to_sq(m);

    std::string s;
    s.reserve(5);

    s.push_back(file_char(file_of(f)));
    s.push_back(rank_char(rank_of(f)));
    s.push_back(file_char(file_of(t)));
    s.push_back(rank_char(rank_of(t)));

    int pr = promo_of(m);
    if (pr) {
        char pc = 'q';
        if (pr == 1) pc = 'n';
        else if (pr == 2) pc = 'b';
        else if (pr == 3) pc = 'r';
        else pc = 'q';
        s.push_back(pc);
    }
    return s;
}

// =====================================
// move existence / legality
// =====================================
inline bool move_exists_pseudo(const Position& pos, Move m) {
    if (!m) return false;
    std::vector<Move> tmp;
    movegen::generate_pseudo_legal(pos, tmp);
    for (Move x : tmp) if (x == m) return true;
    return false;
}

inline bool is_move_legal_fast(Position& pos, Move m) {
    Color us = pos.side;

    if (flags_of(m) & MF_CASTLE) {
        if (!movegen::legal_castle_path_ok(pos, m)) return false;
    }

    Undo u = pos.do_move(m);
    bool ok = !attacks::in_check(pos, us);
    pos.undo_move(m, u);
    return ok;
}

// =======================================
// Hash TT sampling
// =======================================
inline int hashfull_permille_fallback(const TT& tt) {
    if (tt.table.empty()) return 0;

    const size_t N = tt.table.size();
    const size_t SAMPLE = std::min<size_t>(N, 1u << 15);

    size_t filled = 0;
    for (size_t i = 0; i < SAMPLE; i++) {
        if (tt.table[i].key != 0) filled++;
    }
    return int((filled * 1000ULL) / SAMPLE);
}

// =====================================
// Searcher
// =====================================
struct Searcher {
    Zobrist zob;
    TT tt;

    Move killer[2][128]{};
    int  history[2][64][64]{};

    uint64_t nodes = 0;

    Searcher() { tt.resize_mb(64); }

    void set_hash_mb(int mb) { tt.resize_mb(std::max(1, mb)); }

    void clear_tt() {
        for (auto& e : tt.table) e = TTEntry{};
    }

    // 统一加点：本线程+全局
    inline void add_node() {
        nodes++;
        g_nodes_total.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t keyStack[256]{};
    int keyPly = 0;

    static constexpr int MAX_PLY = 128;

    std::vector<Move> plyMoves[MAX_PLY];
    std::vector<int>  plyScores[MAX_PLY];

    struct QNode {
        Move m;
        int  key;
        bool cap;
        bool promo;
        bool check;
    };
    std::vector<QNode> plyQList[MAX_PLY];

    // ====== capture / quiet ======
    inline bool is_capture(const Position& pos, Move m) {
        if (flags_of(m) & MF_EP) return true;
        int to = to_sq(m);
        return pos.board[to] != NO_PIECE;
    }

    inline bool is_quiet(const Position& pos, Move m) {
        return !is_capture(pos, m) && promo_of(m) == 0;
    }

    // ====== MVV-LVA ======
    inline int mvv_lva(Piece victim, Piece attacker) {
        static const int V[7] = {0,100,320,330,500,900,0};
        int vv = (victim == NO_PIECE) ? 0 : V[type_of(victim)];
        int aa = (attacker == NO_PIECE) ? 0 : V[type_of(attacker)];
        return vv * 10 - aa;
    }

    // ====== simple SEE ======
    inline int see(const Position& pos, Move m) {
        int from = from_sq(m);
        int to   = to_sq(m);

        Piece a = pos.board[from];
        Piece v = pos.board[to];

        static const int V[7] = {0,100,320,330,500,900,0};

        int av = V[type_of(a)];
        int vv = (v == NO_PIECE ? 0 : V[type_of(v)]);

        if (flags_of(m) & MF_EP) vv = 100;
        if (promo_of(m)) vv += 800;

        return vv - av;
    }

    // ====== move ordering ======
    inline int move_score(const Position& pos, Move m, Move ttMove, int ply) {
        ply = std::min(ply, 127);

        if (m == ttMove) return 1000000000;

        int from = from_sq(m), to = to_sq(m);

        Piece mover = pos.board[from];

        int sc = 0;

        if (promo_of(m)) sc += 90000000;
        if (flags_of(m) & MF_CASTLE) sc += 30000000;

        if (is_capture(pos, m)) {
            sc += 50000000;

            // victim (EP fixes)
            Piece victim = pos.board[to];
            if (flags_of(m) & MF_EP)
                victim = make_piece(flip_color(pos.side), PAWN);

            int s = see(pos, m);

            if (promo_of(m) || s < -250)
                s = see_full(pos, m);

            s = clampi(s, -500, 500);
            sc += s * 8000;

            sc += mvv_lva(victim, mover) * 200;
            return sc;
        }

        if (m == killer[0][ply]) sc += 20000000;
        else if (m == killer[1][ply]) sc += 15000000;

        sc += history[color_index(pos.side)][from][to];

        if (type_of(mover) == BISHOP) sc += 2000;
        if (type_of(mover) == KNIGHT) sc += 1000;

        if (type_of(mover) == KING && !(flags_of(m) & MF_CASTLE)) {
            if (ply < 12) sc -= 8000000;
            else sc -= 800000;
        }

        if (ply < 4 && type_of(mover) == PAWN && promo_of(m) == 0) {
            if (from == E2 && to == E4) sc += 12000;
            if (from == D2 && to == D4) sc += 12000;
            if (from == E7 && to == E5) sc += 12000;
            if (from == D7 && to == D5) sc += 12000;

            if (from == C2 && to == C4) sc += 7000;
            if (from == C7 && to == C5) sc += 7000;
        }

        return sc;
    }

    inline void pick_best(std::vector<Move>& mv, std::vector<int>& sc, int i) {
        int best = i;
        int bestSc = sc[i];
        for (int j = i + 1; j < (int)mv.size(); j++) {
            if (sc[j] > bestSc) {
                bestSc = sc[j];
                best = j;
            }
        }
        if (best != i) {
            std::swap(mv[i], mv[best]);
            std::swap(sc[i], sc[best]);
        }
    }

    // =====================================
    // PV build (SAFE)  — 你现在没直接用，保留
    // =====================================
    inline std::vector<Move> build_pv_safe(Position pos, Move firstMove, int maxLen = 32) {
        std::vector<Move> pv;
        pv.reserve(maxLen);

        if (!move_exists_pseudo(pos, firstMove)) return pv;
        if (!is_move_legal_fast(pos, firstMove)) return pv;

        uint64_t seen[128];
        int seenN = 0;

        auto seen_before = [&](uint64_t k) {
            for (int i = 0; i < seenN; i++)
                if (seen[i] == k) return true;
            return false;
        };

        seen[seenN++] = zob.key(pos);

        Move cur = firstMove;

        for (int i = 0; i < maxLen; i++) {
            if (!move_exists_pseudo(pos, cur)) break;
            if (!is_move_legal_fast(pos, cur)) break;

            if (!pv.empty()) {
                Move prev = pv.back();
                if (from_sq(cur) == to_sq(prev) && to_sq(cur) == from_sq(prev) &&
                    promo_of(cur) == 0 && promo_of(prev) == 0) {
                    break;
                }
            }

            pv.push_back(cur);

            Color us = pos.side;
            Undo u = pos.do_move(cur);

            if (attacks::in_check(pos, us)) {
                pos.undo_move(cur, u);
                pv.pop_back();
                break;
            }

            uint64_t key = zob.key(pos);

            if (seen_before(key)) {
                pos.undo_move(cur, u);
                pv.pop_back();
                break;
            }
            if (seenN < 128) seen[seenN++] = key;

            TTEntry* e = tt.probe(key);
            if (!e || e->key != key || !e->best) break;

            Move nm = e->best;

            if (!move_exists_pseudo(pos, nm)) break;
            if (!is_move_legal_fast(pos, nm)) break;

            cur = nm;
        }

        return pv;
    }

    // =====================================
    // qsearch (with recapture priority)
    // =====================================
    int qsearch(Position& pos, int alpha, int beta, int ply, int lastTo, bool lastWasCap) {
        add_node();
        selDepth = std::max(selDepth, ply);

        const Color us = pos.side;
        const bool inCheck = attacks::in_check(pos, us);

        if (ply >= 64) return eval::evaluate(pos);

        int stand = -INF;
        if (!inCheck) {
            stand = eval::evaluate(pos);
            if (stand >= beta) return beta;
            if (stand > alpha) alpha = stand;
        }

        auto& moves = plyMoves[ply];
        moves.clear();
        movegen::generate_pseudo_legal(pos, moves);

        auto& list = plyQList[ply];
        list.clear();
        list.reserve(moves.size());

        static const int PVV[7] = {0, 100, 320, 330, 500, 900, 0};

        constexpr int QUIET_CHECK_MAX_PLY = 2;
        constexpr int DELTA_MARGIN = 140;
        constexpr int SEE_CUT = -120;
        constexpr int SEE_FULL_TRIGGER = -200;

        const bool shallow = (ply <= 1);

        for (Move m : moves) {
            if (!move_sane_basic(pos, m)) continue;
            if (flags_of(m) & MF_CASTLE) continue;

            const bool promo = (promo_of(m) != 0);
            const bool cap   = is_capture(pos, m) || (flags_of(m) & MF_EP);

            const bool quietCandidate = (!inCheck && !cap && !promo && ply < QUIET_CHECK_MAX_PLY);
            if (!inCheck && !(cap || promo || quietCandidate)) continue;

            int gain = 0;
            if (cap) {
                int to = to_sq(m);
                if (flags_of(m) & MF_EP) gain += 100;
                else {
                    Piece victim = pos.board[to];
                    gain += (victim == NO_PIECE ? 0 : PVV[type_of(victim)]);
                }
            }
            if (promo) {
                int pr = promo_of(m);
                int newv = (pr == 1 ? 320 : pr == 2 ? 330 : pr == 3 ? 500 : 900);
                gain += (newv - 100);
            }

            if (!inCheck && (cap || promo) && !shallow) {
                if (!promo && stand + gain + DELTA_MARGIN <= alpha) continue;

                if (!promo) {
                    int sQ = see(pos, m);
                    if (sQ <= SEE_FULL_TRIGGER) {
                        int sF = see_full(pos, m);
                        if (sF < SEE_CUT) continue;
                    } else if (sQ < SEE_CUT) {
                        continue;
                    }
                }
            }

            if (quietCandidate && !shallow) {
                if (stand + 40 < alpha) continue;
            }

            int key = 0;
            if (promo) key += 400000;
            if (cap)   key += 80000;
            key += gain * 300;

            // ✅ Recapture priority
            if (lastWasCap && cap && lastTo >= 0 && to_sq(m) == lastTo) {
                key += 220000;
            }

            list.push_back(QNode{m, key, cap, promo, false});
        }

        if (list.empty()) {
            if (inCheck) return -MATE + ply;
            return alpha;
        }

        std::sort(list.begin(), list.end(),
            [](const QNode& a, const QNode& b){ return a.key > b.key; });

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

            const int  nextLastTo     = to_sq(m);
            const bool nextLastWasCap = qn.cap;

            int score = -qsearch(pos, -beta, -alpha, ply + 1, nextLastTo, nextLastWasCap);

            pos.undo_move(m, u);

            if (score >= beta) return beta;
            if (score > alpha) alpha = score;
        }

        return alpha;
    }

    struct NullMoveUndo {
        int   ep;
        Color side;
    };

    inline void do_null_move(Position& pos, NullMoveUndo& u) {
        u.ep   = pos.epSquare;
        u.side = pos.side;

        pos.epSquare = -1;
        pos.side = (pos.side == WHITE) ? BLACK : WHITE;
    }

    inline void undo_null_move(Position& pos, const NullMoveUndo& u) {
        pos.epSquare = u.ep;
        pos.side     = u.side;
    }

    inline bool has_non_pawn_material(const Position& pos, Color c) {
        for (int sq = 0; sq < 64; sq++) {
            Piece p = pos.board[sq];
            if (p == NO_PIECE) continue;

            if (c == WHITE) {
                int v = (int)p;
                if (v >= 1 && v <= 6) {
                    PieceType pt = type_of(p);
                    if (pt != PAWN && pt != KING) return true;
                }
            } else {
                int v = (int)p;
                if (v >= 9 && v <= 14) {
                    PieceType pt = type_of(p);
                    if (pt != PAWN && pt != KING) return true;
                }
            }
        }
        return false;
    }

    // =====================================
    // negamax (with lastTo/lastWasCap)
    // =====================================
    int negamax(Position& pos, int depth, int alpha, int beta, int ply, int lastTo, bool lastWasCap) {
        if (time_up()) return alpha;

        add_node();
        selDepth = std::max(selDepth, ply);

        if (ply >= 128) return eval::evaluate(pos);

        const Color us = pos.side;
        const bool inCheck = attacks::in_check(pos, us);

        // Mate distance pruning
        alpha = std::max(alpha, -MATE + ply);
        beta  = std::min(beta,  MATE - ply - 1);
        if (alpha >= beta) return alpha;

        const uint64_t key = zob.key(pos);

        // repetition: 只扫隔 ply
        if (ply > 0) {
            for (int i = keyPly - 2; i >= 0; i -= 2) {
                if (keyStack[i] == key) return 0;
            }
        }

        keyStack[keyPly++] = key;
        struct KeyPop { int& p; KeyPop(int& x): p(x) {} ~KeyPop(){ p--; } } _kp(keyPly);

        TTEntry* e = tt.probe(key);
        Move ttMove = 0;
        if (e && e->key == key) {
            ttMove = e->best;

            if (e->depth >= depth) {
                int ttScore = from_tt_score((int)e->score, ply);
                if (e->flag == TT_EXACT) return ttScore;
                if (e->flag == TT_ALPHA && ttScore <= alpha) return alpha;
                if (e->flag == TT_BETA  && ttScore >= beta)  return beta;
            }
        }

        // Check extension
        if (inCheck) depth++;

        if (depth <= 0) return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);

        int staticEval = inCheck ? -INF : eval::evaluate(pos);

        // razor
        if (!inCheck && ply > 0 && depth <= 2) {
            const int razorMargin = (depth == 1 ? 220 : 320);
            if (staticEval + razorMargin <= alpha) {
                return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);
            }
        }

        // reverse futility (RFP)
        if (!inCheck && depth <= 3 && ply > 0) {
            const int rfpMargin = 120 + 90 * depth;
            if (staticEval - rfpMargin >= beta) return beta;
        }

        // null move pruning
        if (!inCheck && depth >= 3 && ply > 0) {
            if (has_non_pawn_material(pos, us)) {
                int R = 3 + depth / 6;
                R = std::min(R, depth - 1);

                if (beta < MATE - 256 && alpha > -MATE + 256) {
                    NullMoveUndo nu;
                    do_null_move(pos, nu);

                    int score = -negamax(pos, depth - 1 - R, -beta, -beta + 1,
                                        ply + 1, -1, false);

                    undo_null_move(pos, nu);

                    if (time_up()) return alpha;
                    if (score >= beta) return beta;
                }
            }
        }

        auto& moves  = plyMoves[ply];
        auto& scores = plyScores[ply];
        moves.clear();
        scores.clear();

        movegen::generate_pseudo_legal(pos, moves);

        if (moves.empty()) {
            if (inCheck) return -MATE + ply;
            return 0;
        }

        scores.resize(moves.size());
        for (int i = 0; i < (int)moves.size(); i++)
            scores[i] = move_score(pos, moves[i], ttMove, ply);

        int bestScore = -INF;
        Move bestMove = 0;
        const int origAlpha = alpha;

        bool foundLegal = false;
        int legalMovesSearched = 0;
        int quietMovesSearched = 0;

        for (int i = 0; i < (int)moves.size(); i++) {
            pick_best(moves, scores, i);
            Move m = moves[i];

            if (!move_sane_basic(pos, m)) continue;

            if (flags_of(m) & MF_CASTLE) {
                if (!movegen::legal_castle_path_ok(pos, m)) continue;
            }

            const bool isCap   = is_capture(pos, m) || (flags_of(m) & MF_EP);
            const bool isPromo = (promo_of(m) != 0);
            const bool isQuiet = (!isCap && !isPromo);

            // futility prune (quiet)
            if (!inCheck && isQuiet && ply > 0 && depth <= 2 && m != ttMove) {
                const int futMargin = (depth == 1) ? 190 : 290;
                if (staticEval + futMargin <= alpha) continue;
            }

            // late move prune (very shallow)
            if (!inCheck && isQuiet && ply > 0 && depth <= 2 && m != ttMove) {
                const int limit = (depth == 1) ? 5 : 8;
                if (quietMovesSearched >= limit) continue;
            }

            // capture SEE prune
            if (!inCheck && isCap && !isPromo && ply > 0 && depth <= 4 && m != ttMove) {
                int sQ = see(pos, m);
                if (sQ < -200) {
                    int sF = see_full(pos, m);
                    if (sF < -120) continue;
                } else if (sQ < -120) {
                    continue;
                }
            }

            Undo u = pos.do_move(m);

            // legality
            if (attacks::in_check(pos, us)) {
                pos.undo_move(m, u);
                continue;
            }

            foundLegal = true;
            legalMovesSearched++;
            if (isQuiet) quietMovesSearched++;

            const int  nextLastTo     = to_sq(m);
            const bool nextLastWasCap = isCap;

            int score;

            // PVS + LMR
            if (legalMovesSearched == 1) {
                score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, nextLastTo, nextLastWasCap);
            } else {
                int reduction = 0;
                if (depth >= 3 && !inCheck && isQuiet && ply > 0) {
                    reduction = 1;
                    if (legalMovesSearched > 4)  reduction++;
                    if (legalMovesSearched > 10) reduction++;
                    if (depth >= 7 && legalMovesSearched > 14) reduction++;
                    reduction = std::min(reduction, depth - 2);
                }

                int rd = depth - 1 - reduction;
                if (rd < 0) rd = 0;

                score = -negamax(pos, rd, -alpha - 1, -alpha, ply + 1, nextLastTo, nextLastWasCap);

                if (score > alpha && reduction > 0 && rd != depth - 1) {
                    score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1, nextLastTo, nextLastWasCap);
                }

                if (score > alpha && score < beta) {
                    score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, nextLastTo, nextLastWasCap);
                }
            }

            pos.undo_move(m, u);

            if (time_up()) return alpha;

            if (score > bestScore) {
                bestScore = score;
                bestMove = m;
            }

            if (score > alpha) alpha = score;

            if (alpha >= beta) {
                if (isQuiet && ply < 128) {
                    if (killer[0][ply] != m) {
                        killer[1][ply] = killer[0][ply];
                        killer[0][ply] = m;
                    }
                    int from = from_sq(m), to = to_sq(m);
                    int ci = color_index(us);
                    history[ci][from][to] += depth * depth * 32;
                    if (history[ci][from][to] > 300000) history[ci][from][to] = 300000;
                }
                break;
            }
        }

        if (!foundLegal) {
            if (inCheck) return -MATE + ply;
            return 0;
        }

        // store TT (单线程 TT：安全)
        if (e) {
            TTFlag flag = TT_EXACT;
            if (bestScore <= origAlpha) flag = TT_ALPHA;
            else if (bestScore >= beta) flag = TT_BETA;

            int store = clampi(bestScore, -INF, INF);
            store = to_tt_score(store, ply);

            e->best  = bestMove;
            e->score = (int16_t)store;
            e->depth = (int16_t)depth;
            e->flag  = (uint8_t)flag;
            e->key   = key;
        }

        return bestScore;
    }

    static inline bool is_legal_move_here(Position& pos, Move m) {
        if (!move_sane_basic(pos, m)) return false;

        if (flags_of(m) & MF_CASTLE) {
            if (!movegen::legal_castle_path_ok(pos, m)) return false;
        }

        const Color us = pos.side;
        Undo u = pos.do_move(m);
        const bool ok = !attacks::in_check(pos, us);
        pos.undo_move(m, u);
        return ok;
    }

    std::vector<Move> pv_from_tt(Position& pos, int maxLen);

    // =====================================
    // think (emitInfo: only main thread prints)
    // =====================================
    Result think(Position& pos, const Limits& lim, bool emitInfo) {
        keyPly = 0;
        keyStack[keyPly++] = zob.key(pos);

        Result res{};
        nodes = 0;
        selDepth = 0;

        const int maxDepth = (lim.depth > 0 ? lim.depth : 64);
        const int64_t startT = now_ms();

        std::vector<Move> rootMoves;
        movegen::generate_legal(pos, rootMoves);

        if (rootMoves.empty()) {
            res.bestMove = 0;
            res.score = 0;
            res.nodes = nodes;
            return res;
        }

        Move bestMove  = rootMoves[0];
        int  bestScore = -INF;

        constexpr int ASP_START = 35;
        constexpr int ROOT_EARLY_CUT = 40;
        constexpr int PV_MAX = 12;
        constexpr int ROOT_ORDER_K = 10;

        auto now_time_nodes_nps = [&]() {
            int t = (int)(now_ms() - startT);
            if (t < 1) t = 1;

            uint64_t nodesAll = g_nodes_total.load(std::memory_order_relaxed);
            uint64_t nps = (nodesAll * 1000ULL) / (uint64_t)t;

            return std::tuple<int, uint64_t, uint64_t>(t, nps, nodesAll);
        };

        auto print_currmove = [&](int depth, int sd, Move m, int moveNo) {
            if (!emitInfo) return;
            auto [t, nps, nodesAll] = now_time_nodes_nps();
            std::cout << "info depth " << depth
                    << " seldepth " << sd
                    << " time " << t
                    << " nodes " << nodesAll
                    << " nps " << nps
                    << " currmove " << move_to_uci(m)
                    << " currmovenumber " << moveNo
                    << "\n";
            std::cout.flush();
        };

        auto root_search = [&](int d, int alpha, int beta, Move& outBestMove, int& outBestScore, bool& outOk) {
            outOk = true;

            int curAlpha = alpha;
            int curBeta  = beta;

            Move iterBestMove  = 0;
            int  iterBestScore = -INF;

            int rootLegalsSearched = 0;

            // Stockfish-ish currmove checkpoints (少量)
            std::vector<int> checkpoints;
            if (emitInfo && d >= 6) {
                int n = (int)rootMoves.size();
                auto add_cp = [&](int idx) {
                    idx = std::max(0, std::min(n - 1, idx));
                    if (checkpoints.empty() || checkpoints.back() != idx) checkpoints.push_back(idx);
                };
                add_cp(0);
                add_cp(n / 4);
                add_cp(n / 2);
                add_cp((n * 3) / 4);
                std::sort(checkpoints.begin(), checkpoints.end());
                checkpoints.erase(std::unique(checkpoints.begin(), checkpoints.end()), checkpoints.end());
            }
            int cpPtr = 0;

            for (int i = 0; i < (int)rootMoves.size(); i++) {
                if (time_up()) { outOk = false; break; }

                Move m = rootMoves[i];

                if (cpPtr < (int)checkpoints.size() && i == checkpoints[cpPtr]) {
                    int sd = std::max(1, selDepth);
                    print_currmove(d, sd, m, i + 1);
                    cpPtr++;
                }

                const bool isCap   = is_capture(pos, m) || (flags_of(m) & MF_EP);
                const bool isPromo = (promo_of(m) != 0);

                Undo u = pos.do_move(m);

                // root safety
                if (attacks::in_check(pos, flip_color(pos.side))) {
                    pos.undo_move(m, u);
                    continue;
                }

                rootLegalsSearched++;

                bool givesCheck = false;
                if (d >= 6 && i >= 4) {
                    givesCheck = attacks::in_check(pos, pos.side);
                }

                const bool isQuiet  = (!isCap && !isPromo && !givesCheck);

                const int  nextLastTo     = to_sq(m);
                const bool nextLastWasCap = isCap;

                int score = -INF;

                int r = 0;
                if (isQuiet && d >= 6 && i >= 4) {
                    r = 1;
                    if (d >= 10 && i >= 10) r = 2;
                    r = std::min(r, d - 2);
                }

                if (rootLegalsSearched == 1) {
                    score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, nextLastTo, nextLastWasCap);
                } else {
                    int rd = (d - 1) - r;
                    if (rd < 0) rd = 0;

                    score = -negamax(pos, rd, -curAlpha - 1, -curAlpha, 1, nextLastTo, nextLastWasCap);

                    //if (isQuiet && i >= 6 && score <= curAlpha - ROOT_EARLY_CUT) {
                    //    pos.undo_move(m, u);
                    //    continue;
                    //}

                    if (score > curAlpha && score < curBeta) {
                        score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, nextLastTo, nextLastWasCap);
                    }
                }

                pos.undo_move(m, u);

                if (time_up()) { outOk = false; break; }

                if (score > iterBestScore) {
                    iterBestScore = score;
                    iterBestMove  = m;
                }

                if (score > curAlpha) curAlpha = score;
                if (curAlpha >= curBeta) break;
            }

            if (rootLegalsSearched == 0) outOk = false;

            outBestMove  = iterBestMove;
            outBestScore = iterBestScore;
        };

        for (int d = 1; d <= maxDepth; d++) {
            if (time_up()) break;

            if (bestMove) {
                auto it = std::find(rootMoves.begin(), rootMoves.end(), bestMove);
                if (it != rootMoves.end()) std::swap(rootMoves[0], *it);
            }

            std::vector<int> order(rootMoves.size());
            for (int i = 0; i < (int)rootMoves.size(); i++)
                order[i] = move_score(pos, rootMoves[i], bestMove, 0);

            const int K = std::min<int>(ROOT_ORDER_K, (int)rootMoves.size());
            for (int i = 0; i < K; i++) {
                int bi = i;
                int bs = order[i];
                for (int j = i + 1; j < (int)rootMoves.size(); j++) {
                    if (order[j] > bs) { bs = order[j]; bi = j; }
                }
                if (bi != i) {
                    std::swap(rootMoves[i], rootMoves[bi]);
                    std::swap(order[i], order[bi]);
                }
            }

            const bool useAsp = (d > 5 && bestScore > -INF/2 && bestScore < INF/2);
            int alpha = useAsp ? (bestScore - ASP_START) : -INF;
            int beta  = useAsp ? (bestScore + ASP_START) :  INF;

            Move localBestMove  = bestMove;
            int  localBestScore = bestScore;
            bool ok = false;

            root_search(d, alpha, beta, localBestMove, localBestScore, ok);
            if (!ok) break;

            if (useAsp && (localBestScore <= alpha || localBestScore >= beta)) {
                alpha = -INF;
                beta  =  INF;
                root_search(d, alpha, beta, localBestMove, localBestScore, ok);
                if (!ok) break;
            }

            bestMove  = localBestMove;
            bestScore = localBestScore;

            std::vector<Move> pv;
            pv.reserve(PV_MAX);

            if (bestMove) {
                pv.push_back(bestMove);
                if ((int)pv.size() < PV_MAX) {
                    Undo u = pos.do_move(bestMove);
                    auto tail = pv_from_tt(pos, PV_MAX - 1);
                    pos.undo_move(bestMove, u);

                    for (Move tm : tail) {
                        if (!tm) break;
                        pv.push_back(tm);
                        if ((int)pv.size() >= PV_MAX) break;
                    }
                }
            }

            if (emitInfo) {
                auto [t, nps, nodesAll] = now_time_nodes_nps();
                int hashfull = hashfull_permille_fallback(tt);
                int sd = std::max(1, selDepth);

                std::cout << "info depth " << d
                        << " seldepth " << sd
                        << " multipv 1"
                        << " ";
                print_score_uci(bestScore);
                std::cout << " nodes " << nodesAll
                        << " nps " << nps
                        << " hashfull " << hashfull
                        << " tbhits 0"
                        << " time " << t
                        << " pv ";

                for (Move pm : pv) {
                    if (pm) std::cout << move_to_uci(pm) << " ";
                }
                std::cout << "\n";
                std::cout.flush();
            }
        }

        res.bestMove = bestMove;
        res.score = bestScore;
        res.nodes = nodes;
        return res;
    }
};

// =====================================
// pv_from_tt (outside class)
// =====================================
inline std::vector<Move> search::Searcher::pv_from_tt(Position& pos, int maxLen) {
    std::vector<Move> pv;
    pv.reserve(maxLen);

    Undo undos[128];
    Move moves[128];
    int ucnt = 0;

    uint64_t seen[128];
    int seenN = 0;

    auto seen_before = [&](uint64_t k) {
        for (int i = 0; i < seenN; i++) if (seen[i] == k) return true;
        return false;
    };

    Move prev = 0;

    for (int i = 0; i < maxLen; i++) {
        const uint64_t key = zob.key(pos);

        if (seen_before(key)) break;
        if (seenN < 128) seen[seenN++] = key;

        TTEntry* e = tt.probe(key);
        if (!e || e->key != key) break;

        if (e->flag != TT_EXACT) break;

        Move m = e->best;
        if (!m) break;

        if (!is_legal_move_here(pos, m)) break;

        if (prev &&
            from_sq(m) == to_sq(prev) &&
            to_sq(m) == from_sq(prev) &&
            promo_of(m) == 0 && promo_of(prev) == 0) {
            break;
        }

        moves[ucnt] = m;
        undos[ucnt] = pos.do_move(m);
        ucnt++;

        if (attacks::in_check(pos, flip_color(pos.side))) {
            pos.undo_move(m, undos[--ucnt]);
            break;
        }

        pv.push_back(m);
        prev = m;
    }

    for (int i = ucnt - 1; i >= 0; i--) {
        pos.undo_move(moves[i], undos[i]);
    }

    return pv;
}

// =====================================
// THREADS (Lazy SMP) — minimal + stable
// =====================================
inline std::atomic<int> g_threads{1};
inline int g_hash_mb = 64;

inline std::vector<std::unique_ptr<Searcher>> g_pool_owner;
inline std::vector<Searcher*> g_pool;

inline void set_threads(int n) {
    n = std::max(1, std::min(256, n));
    g_threads.store(n, std::memory_order_relaxed);

    g_pool_owner.clear();
    g_pool_owner.reserve(n);
    g_pool.clear();
    g_pool.reserve(n);

    for (int i = 0; i < n; i++) {
        g_pool_owner.emplace_back(std::make_unique<Searcher>());
        g_pool_owner.back()->set_hash_mb(g_hash_mb);
        g_pool.push_back(g_pool_owner.back().get());
    }
}

inline int threads() {
    return g_threads.load(std::memory_order_relaxed);
}

inline void ensure_pool() {
    if (g_pool.empty()) set_threads(1);
}

// =====================================
// global API
// =====================================
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

    // 副线程静默跑（Position 拷贝）
    for (int i = 1; i < n; i++) {
        Position pcopy = pos;
        workers.emplace_back([i, pcopy, lim]() mutable {
            g_pool[i]->think(pcopy, lim, false);
        });
    }

    // 主线程输出
    Result mainRes = g_pool[0]->think(pos, lim, true);

    stop();
    for (auto& th : workers) th.join();

    mainRes.nodes = g_nodes_total.load(std::memory_order_relaxed);
    return mainRes;
}

inline void set_hash_mb(int mb) {
    ensure_pool();
    g_hash_mb = std::max(1, mb);
    for (auto* s : g_pool) s->set_hash_mb(g_hash_mb);
}

inline void clear_tt() {
    ensure_pool();
    for (auto* s : g_pool) s->clear_tt();
}

} // namespace search
