// Pseudo-legal move generation and legality filtering.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

#include "types.h"
#include "Position.h"
#include "Attack.h"

namespace movegen {

// Basic helpers for board boundaries.
inline bool on_board(int sq) {
    return sq >= 0 && sq < 64;
}
inline bool same_rank(int a, int b) {
    return rank_of(a) == rank_of(b);
}
inline bool diag_step_ok(int from, int to) {
    return std::abs(file_of(to) - file_of(from)) == 1;
}

// Push a move if target is on board and not occupied by our own piece.
// Sets capture flag automatically when destination is occupied.
inline void push_move(const Position& pos, std::vector<Move>& moves, int from, int to, int flags = 0, int promo = 0) {
    if (!on_board(to))
        return;

    Piece dst = pos.board[to];
    if (same_color(dst, pos.side))
        return;

    // auto capture flag
    if (dst != NO_PIECE)
        flags |= MF_CAPTURE;

    moves.push_back(make_move(from, to, flags, promo));
}

inline void add_promo(const Position& pos, std::vector<Move>& moves, int from, int to, int flags = 0) {
    // promo: 1=N 2=B 3=R 4=Q
    push_move(pos, moves, from, to, flags | MF_PROMO, 1);
    push_move(pos, moves, from, to, flags | MF_PROMO, 2);
    push_move(pos, moves, from, to, flags | MF_PROMO, 3);
    push_move(pos, moves, from, to, flags | MF_PROMO, 4);
}

// Pseudo-legal generator (includes castling and en passant).
// Does not filter out moves that leave the king in check.
inline void generate_pseudo_legal(const Position& pos, std::vector<Move>& moves) {
    moves.clear();
    if (moves.capacity() < 256)
        moves.reserve(256);
    Color us = pos.side;

    static const int N_OFF[8] = {+17, +15, +10, +6, -6, -10, -15, -17};
    static const int K_OFF[8] = {+8, -8, +1, -1, +9, +7, -7, -9};

    static const int DIR_B[4] = {+9, +7, -7, -9};
    static const int DIR_R[4] = {+8, -8, +1, -1};

    for (int sq = 0; sq < 64; sq++) {
        Piece p = pos.board[sq];
        if (p == NO_PIECE)
            continue;
        if (color_of(p) != us)
            continue;

        PieceType pt = type_of(p);

        // ---------------------
        // PAWN
        // ---------------------
        if (pt == PAWN) {
            const int dir = (us == WHITE) ? +8 : -8;
            const int startRank = (us == WHITE) ? 1 : 6;
            const int promoFromRank = (us == WHITE) ? 6 : 1;

            int one = sq + dir;
            if (on_board(one) && pos.board[one] == NO_PIECE) {
                if (rank_of(sq) == promoFromRank)
                    add_promo(pos, moves, sq, one);
                else
                    push_move(pos, moves, sq, one);

                if (rank_of(sq) == startRank) {
                    int two = sq + dir * 2;
                    if (on_board(two) && pos.board[two] == NO_PIECE)
                        push_move(pos, moves, sq, two);
                }
            }

            int capL = (us == WHITE) ? (sq + 7) : (sq - 9);
            int capR = (us == WHITE) ? (sq + 9) : (sq - 7);

            if (file_of(sq) != 0 && on_board(capL) && enemy_color(pos.board[capL], us)) {
                if (rank_of(sq) == promoFromRank)
                    add_promo(pos, moves, sq, capL);
                else
                    push_move(pos, moves, sq, capL);
            }
            if (file_of(sq) != 7 && on_board(capR) && enemy_color(pos.board[capR], us)) {
                if (rank_of(sq) == promoFromRank)
                    add_promo(pos, moves, sq, capR);
                else
                    push_move(pos, moves, sq, capR);
            }

            // EP
            if (pos.epSquare != -1) {
                if (file_of(sq) != 0 && capL == pos.epSquare) {
                    push_move(pos, moves, sq, capL, MF_EP);
                }
                if (file_of(sq) != 7 && capR == pos.epSquare) {
                    push_move(pos, moves, sq, capR, MF_EP);
                }
            }

            continue;
        }

        // ---------------------
        // KNIGHT
        // ---------------------
        if (pt == KNIGHT) {
            int f = file_of(sq), r = rank_of(sq);
            for (int k = 0; k < 8; k++) {
                int to = sq + N_OFF[k];
                if (!on_board(to))
                    continue;

                int tf = file_of(to), tr = rank_of(to);
                int df = std::abs(tf - f), dr = std::abs(tr - r);
                if (!((df == 1 && dr == 2) || (df == 2 && dr == 1)))
                    continue;

                push_move(pos, moves, sq, to);
            }
            continue;
        }

        // ---------------------
        // KING (+ castling pseudo)
        // ---------------------
        if (pt == KING) {
            int f = file_of(sq), r = rank_of(sq);

            for (int k = 0; k < 8; k++) {
                int to = sq + K_OFF[k];
                if (!on_board(to))
                    continue;

                int tf = file_of(to), tr = rank_of(to);
                if (std::abs(tf - f) > 1 || std::abs(tr - r) > 1)
                    continue;

                push_move(pos, moves, sq, to);
            }

            // castling pseudo (rights + empty path)
            if (us == WHITE && sq == E1) {
                if ((pos.castlingRights & CR_WK) && pos.board[F1] == NO_PIECE && pos.board[G1] == NO_PIECE) {
                    push_move(pos, moves, E1, G1, MF_CASTLE, 0);
                }
                if ((pos.castlingRights & CR_WQ) && pos.board[D1] == NO_PIECE && pos.board[C1] == NO_PIECE &&
                    pos.board[B1] == NO_PIECE) {
                    push_move(pos, moves, E1, C1, MF_CASTLE, 0);
                }
            } else if (us == BLACK && sq == E8) {
                if ((pos.castlingRights & CR_BK) && pos.board[F8] == NO_PIECE && pos.board[G8] == NO_PIECE) {
                    push_move(pos, moves, E8, G8, MF_CASTLE, 0);
                }
                if ((pos.castlingRights & CR_BQ) && pos.board[D8] == NO_PIECE && pos.board[C8] == NO_PIECE &&
                    pos.board[B8] == NO_PIECE) {
                    push_move(pos, moves, E8, C8, MF_CASTLE, 0);
                }
            }

            continue;
        }

        // ---------------------
        // SLIDERS: bishop / rook / queen
        // ---------------------
        auto slide = [&](const int* dirs, int cnt) {
            for (int i = 0; i < cnt; i++) {
                int d = dirs[i];
                int cur = sq;
                while (true) {
                    int to = cur + d;
                    if (!on_board(to))
                        break;

                    if ((d == +1 || d == -1) && !same_rank(cur, to))
                        break;
                    if ((d == +9 || d == -9 || d == +7 || d == -7) && !diag_step_ok(cur, to))
                        break;

                    if (pos.board[to] == NO_PIECE) {
                        push_move(pos, moves, sq, to);
                        cur = to;
                        continue;
                    }

                    if (enemy_color(pos.board[to], us))
                        push_move(pos, moves, sq, to);

                    break;
                }
            }
        };

        if (pt == BISHOP)
            slide(DIR_B, 4);
        else if (pt == ROOK)
            slide(DIR_R, 4);
        else if (pt == QUEEN) {
            slide(DIR_B, 4);
            slide(DIR_R, 4);
        }
    }
}

// Castling path legality: king cannot move through or into check.
inline bool legal_castle_path_ok(const Position& pos, Move m) {
    int from = from_sq(m);
    int to = to_sq(m);

    Color us = pos.side;
    Color them = ~us;

    if (attacks::in_check(pos, us))
        return false;

    if (us == WHITE) {
        if (from == E1 && to == G1) {
            if (attacks::is_square_attacked(pos, F1, them))
                return false;
            if (attacks::is_square_attacked(pos, G1, them))
                return false;
            return true;
        }
        if (from == E1 && to == C1) {
            if (attacks::is_square_attacked(pos, D1, them))
                return false;
            if (attacks::is_square_attacked(pos, C1, them))
                return false;
            return true;
        }
    } else {
        if (from == E8 && to == G8) {
            if (attacks::is_square_attacked(pos, F8, them))
                return false;
            if (attacks::is_square_attacked(pos, G8, them))
                return false;
            return true;
        }
        if (from == E8 && to == C8) {
            if (attacks::is_square_attacked(pos, D8, them))
                return false;
            if (attacks::is_square_attacked(pos, C8, them))
                return false;
            return true;
        }
    }
    return true;
}

// Legal move generator (used for root and validation).
// Avoid calling in deep search nodes due to cost.
inline void generate_legal(Position& pos, std::vector<Move>& legal) {
    static thread_local std::vector<Move> pseudo;
    generate_pseudo_legal(pos, pseudo);

    legal.clear();
    legal.reserve(pseudo.size());

    Color us = pos.side;

    for (Move m : pseudo) {
        if (flags_of(m) & MF_CASTLE) {
            if (!legal_castle_path_ok(pos, m))
                continue;
        }

        Undo u = pos.do_move(m);
        bool ok = !attacks::in_check(pos, us);
        pos.undo_move(m, u);

        if (ok)
            legal.push_back(m);
    }
}

// Legal captures only (used by quiescence and tactical filters).
inline void generate_legal_captures(Position& pos, std::vector<Move>& caps) {
    static thread_local std::vector<Move> legal;
    legal.clear();
    if (legal.capacity() < 256)
        legal.reserve(256);
    generate_legal(pos, legal);

    caps.clear();
    caps.reserve(legal.size());
    for (Move m : legal) {
        if ((flags_of(m) & MF_CAPTURE) || (flags_of(m) & MF_EP) || promo_of(m))
            caps.push_back(m);
    }
}

} // namespace movegen
