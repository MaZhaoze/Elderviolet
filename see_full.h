#pragma once
#include <cstdint>
#include <algorithm>

#include "types.h"
#include "Position.h"

// ============================================================
// Internal helpers: bitboard on 0..63 squares (a1=0)
// ============================================================
using U64 = uint64_t;

static inline U64 bb_sq(int sq) { return (U64(1) << sq); }

static inline int pop_lsb(U64& b) {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, b);
    b &= b - 1;
    return (int)idx;
#else
    int idx = __builtin_ctzll(b);
    b &= b - 1;
    return idx;
#endif
}

static inline int piece_value_pt(PieceType pt) {
    // 你可调：这里用常见中局子力
    switch (pt) {
    case PAWN:   return 100;
    case KNIGHT: return 320;
    case BISHOP: return 330;
    case ROOK:   return 500;
    case QUEEN:  return 900;
    case KING:   return 20000;
    default:     return 0;
    }
}
static inline int piece_value(Piece p) {
    return piece_value_pt(type_of(p));
}

// 升变编码：0 none, 1N 2B 3R 4Q
static inline PieceType promo_to_pt(int promoCode) {
    if (promoCode == 1) return KNIGHT;
    if (promoCode == 2) return BISHOP;
    if (promoCode == 3) return ROOK;
    if (promoCode == 4) return QUEEN;
    return NONE;
}

// 生成 occupancy（内部 bitboard）
static inline U64 occ_from_board(const Piece board[64]) {
    U64 occ = 0;
    for (int sq = 0; sq < 64; ++sq)
        if (board[sq] != NO_PIECE)
            occ |= bb_sq(sq);
    return occ;
}

// ============================================================
// Attack detection to a square (mailbox scan, returns attackers bitboard)
// ============================================================

static inline bool on_board(int sq) { return (unsigned)sq < 64u; }

static inline void add_pawn_attackers_to(U64& attackers, const Piece b[64], int toSq) {
    // white pawns that attack toSq are located at toSq-7 and toSq-9
    // black pawns that attack toSq are located at toSq+7 and toSq+9
    int f = file_of(toSq);

    // white
    if (f > 0) {
        int s = toSq - 9;
        if (on_board(s) && b[s] == W_PAWN) attackers |= bb_sq(s);
    }
    if (f < 7) {
        int s = toSq - 7;
        if (on_board(s) && b[s] == W_PAWN) attackers |= bb_sq(s);
    }

    // black
    if (f > 0) {
        int s = toSq + 7;
        if (on_board(s) && b[s] == B_PAWN) attackers |= bb_sq(s);
    }
    if (f < 7) {
        int s = toSq + 9;
        if (on_board(s) && b[s] == B_PAWN) attackers |= bb_sq(s);
    }
}

static inline void add_knight_attackers_to(U64& attackers, const Piece b[64], int toSq) {
    static const int offs[8] = {+17,+15,+10,+6,-6,-10,-15,-17};
    int tf = file_of(toSq), tr = rank_of(toSq);

    for (int k = 0; k < 8; ++k) {
        int s = toSq + offs[k];
        if (!on_board(s)) continue;
        int sf = file_of(s), sr = rank_of(s);
        int df = std::abs(sf - tf), dr = std::abs(sr - tr);
        if (!((df == 1 && dr == 2) || (df == 2 && dr == 1))) continue;

        Piece p = b[s];
        if (p == W_KNIGHT || p == B_KNIGHT) attackers |= bb_sq(s);
    }
}

static inline void add_king_attackers_to(U64& attackers, const Piece b[64], int toSq) {
    static const int offs[8] = {+1,-1,+8,-8,+9,+7,-7,-9};
    int tf = file_of(toSq), tr = rank_of(toSq);

    for (int k = 0; k < 8; ++k) {
        int s = toSq + offs[k];
        if (!on_board(s)) continue;
        int sf = file_of(s), sr = rank_of(s);
        if (std::abs(sf - tf) > 1 || std::abs(sr - tr) > 1) continue;

        Piece p = b[s];
        if (p == W_KING || p == B_KING) attackers |= bb_sq(s);
    }
}

static inline void ray_first_attacker(U64& attackers, const Piece b[64], int toSq, int df, int dr, bool diag) {
    // df/dr are step in file/rank per ray
    int f = file_of(toSq);
    int r = rank_of(toSq);

    for (;;) {
        f += df; r += dr;
        if ((unsigned)f >= 8u || (unsigned)r >= 8u) break;
        int s = make_sq(f, r);
        Piece p = b[s];
        if (p == NO_PIECE) continue;

        PieceType pt = type_of(p);
        if (diag) {
            if (pt == BISHOP || pt == QUEEN) attackers |= bb_sq(s);
        } else {
            if (pt == ROOK || pt == QUEEN) attackers |= bb_sq(s);
        }
        break; // first blocker ends ray
    }
}

static inline U64 attackers_to_sq(const Piece b[64], int toSq) {
    U64 att = 0;

    add_pawn_attackers_to(att, b, toSq);
    add_knight_attackers_to(att, b, toSq);
    add_king_attackers_to(att, b, toSq);

    // diagonals
    ray_first_attacker(att, b, toSq, +1, +1, true);
    ray_first_attacker(att, b, toSq, -1, +1, true);
    ray_first_attacker(att, b, toSq, +1, -1, true);
    ray_first_attacker(att, b, toSq, -1, -1, true);

    // orthogonals
    ray_first_attacker(att, b, toSq, +1,  0, false);
    ray_first_attacker(att, b, toSq, -1,  0, false);
    ray_first_attacker(att, b, toSq,  0, +1, false);
    ray_first_attacker(att, b, toSq,  0, -1, false);

    return att;
}

static inline U64 color_attackers(U64 attackers, const Piece b[64], Color c) {
    U64 res = 0;
    U64 tmp = attackers;
    while (tmp) {
        int sq = pop_lsb(tmp);
        Piece p = b[sq];
        if (p != NO_PIECE && color_of(p) == c)
            res |= bb_sq(sq);
    }
    return res;
}

static inline int least_valuable_attacker_sq(U64 attackersSide, const Piece b[64]) {
    // 返回攻击者里“最便宜”的那一个 square；若空返回 -1
    int bestSq = -1;
    int bestV  = 1e9;

    U64 tmp = attackersSide;
    while (tmp) {
        int sq = pop_lsb(tmp);
        Piece p = b[sq];
        if (p == NO_PIECE) continue;
        int v = piece_value(p);
        if (v < bestV) {
            bestV = v;
            bestSq = sq;
        }
    }
    return bestSq;
}

static inline bool pawn_promo_by_move(Color side, int fromSq, int toSq) {
    // pawn diagonal capture to last rank
    int tr = rank_of(toSq);
    if (side == WHITE) {
        return tr == 7 && rank_of(fromSq) == 6;
    } else {
        return tr == 0 && rank_of(fromSq) == 1;
    }
}

// ============================================================
// Stockfish-like swap SEE
// returns net material gain for side to move in pos making move m
// ============================================================
// ============================================================
// Correct swap SEE (Stockfish-style, mailbox attackers recompute)
// returns net gain for side to move if it plays m
// ============================================================
static inline int see_full(const Position& pos, Move m) {
    if (!m) return 0;
    if (flags_of(m) & MF_CASTLE) return 0;

    const int from = from_sq(m);
    const int to   = to_sq(m);
    if ((unsigned)from >= 64u || (unsigned)to >= 64u) return 0;

    Piece board[64];
    for (int i = 0; i < 64; ++i) board[i] = pos.board[i];

    Piece mover = board[from];
    if (mover == NO_PIECE) return 0;

    const Color us = pos.side;

    // ---- value of initially captured piece (EP special) ----
    int capturedV = 0;
    if (flags_of(m) & MF_EP) {
        capturedV = piece_value_pt(PAWN);
    } else {
        capturedV = piece_value(board[to]);
    }

    // ---- apply the initial capture on a copy board ----
    // remove captured
    if (flags_of(m) & MF_EP) {
        int capSq = (us == WHITE) ? (to - 8) : (to + 8);
        if (on_board(capSq)) board[capSq] = NO_PIECE;
    } else {
        board[to] = NO_PIECE;
    }

    // move attacker (handle promotion)
    int pr = promo_of(m);
    board[from] = NO_PIECE;

    Piece placed = mover;
    if (pr) {
        placed = make_piece(us, promo_to_pt(pr));
        // promotion increases what the opponent wins when recapturing this piece
        // (handled below by "value of piece on to" = placed's value)
    }
    board[to] = placed;

    // ---- swap list ----
    // gain[0] is what we won by the initial capture
    int gain[32];
    int d = 0;
    gain[0] = capturedV;

    // Now opponent tries to recapture on `to`
    Color side = (us == WHITE ? BLACK : WHITE);

    // The piece currently sitting on `to` (the one to be captured next)
    Piece onTo = board[to];

    while (true) {
        // find all attackers to `to` for current side
        U64 allAtt   = attackers_to_sq(board, to);
        U64 attSide  = color_attackers(allAtt, board, side);
        if (!attSide) break;

        // pick least valuable attacker square
        int aSq = least_valuable_attacker_sq(attSide, board);
        if (aSq < 0) break;

        Piece aPiece = board[aSq];

        // the value captured this ply is the value of the piece currently on `to`
        int capVal = piece_value(onTo);

        // If current side captures with a pawn onto last rank, assume it promotes to queen
        bool promoCap = (type_of(aPiece) == PAWN && pawn_promo_by_move(side, aSq, to));
        if (promoCap) {
            // the capturing pawn becomes a queen on `to`,
            // BUT the captured value is still the piece on `to` (capVal already correct).
            // Next ply, the opponent will be capturing a queen (handled by onTo update).
        }

        d++;
        if (d >= 31) break;

        // standard SEE recurrence: gain[d] = capVal - gain[d-1]
        gain[d] = capVal - gain[d - 1];

        // apply capture aSq -> to on board
        board[aSq] = NO_PIECE;
        board[to]  = aPiece;

        // handle promotion on the board (default to queen)
        if (promoCap) {
            board[to] = make_piece(side, QUEEN);
        }

        // update onTo: now the piece on `to` is what the other side would capture next
        onTo = board[to];

        // switch side
        side = (side == WHITE ? BLACK : WHITE);
    }

    // ---- backward induction ----
    for (int i = d - 1; i >= 0; --i) {
        gain[i] = std::max(gain[i], -gain[i + 1]);
    }

    return gain[0];
}

static inline bool see_ge(const Position& pos, Move m, int threshold) {
    return see_full(pos, m) >= threshold;
}
