#pragma once
#include <cstdint>
#include "types.h"
#include "Position.h"

namespace attacks {

using Bitboard = uint64_t;

// Precomputed attack tables and helper queries.

static constexpr inline Bitboard bb_sq(int sq) {
    return (sq >= 0 && sq < 64) ? (1ULL << sq) : 0ULL;
}

static constexpr inline Color flip(Color c) {
    return (c == WHITE) ? BLACK : WHITE;
}

// Precomputed leaper attacks (knight/king/pawn).
struct Tables {
    Bitboard knight[64]{};
    Bitboard king[64]{};
    Bitboard pawn[2][64]{}; // pawn[color][sq] = squares attacked by pawn on sq

    Tables() {
        for (int sq = 0; sq < 64; sq++) {
            const int f = file_of(sq);
            const int r = rank_of(sq);

            // Knight
            {
                static const int df[8] = {+1, +2, +2, +1, -1, -2, -2, -1};
                static const int dr[8] = {+2, +1, -1, -2, -2, -1, +1, +2};
                Bitboard b = 0;
                for (int i = 0; i < 8; i++) {
                    int ff = f + df[i];
                    int rr = r + dr[i];
                    if ((unsigned)ff < 8u && (unsigned)rr < 8u) {
                        int to = make_sq(ff, rr);
                        b |= bb_sq(to);
                    }
                }
                knight[sq] = b;
            }

            // King
            {
                static const int df[8] = {+1, +1, 0, -1, -1, -1, 0, +1};
                static const int dr[8] = {0, +1, +1, +1, 0, -1, -1, -1};
                Bitboard b = 0;
                for (int i = 0; i < 8; i++) {
                    int ff = f + df[i];
                    int rr = r + dr[i];
                    if ((unsigned)ff < 8u && (unsigned)rr < 8u) {
                        int to = make_sq(ff, rr);
                        b |= bb_sq(to);
                    }
                }
                king[sq] = b;
            }

            // Pawn
            {
                // White pawns attack up (r+1), black pawns attack down (r-1).
                Bitboard w = 0, b = 0;

                // WHITE: (f-1,r+1) and (f+1,r+1)
                if ((unsigned)(r + 1) < 8u) {
                    if ((unsigned)(f - 1) < 8u)
                        w |= bb_sq(make_sq(f - 1, r + 1));
                    if ((unsigned)(f + 1) < 8u)
                        w |= bb_sq(make_sq(f + 1, r + 1));
                }

                // BLACK: (f-1,r-1) and (f+1,r-1)
                if ((unsigned)(r - 1) < 8u) {
                    if ((unsigned)(f - 1) < 8u)
                        b |= bb_sq(make_sq(f - 1, r - 1));
                    if ((unsigned)(f + 1) < 8u)
                        b |= bb_sq(make_sq(f + 1, r - 1));
                }

                pawn[WHITE][sq] = w;
                pawn[BLACK][sq] = b;
            }
        }
    }
};

// Lazy-initialized attack tables (thread-safe in C++11+).
inline const Tables& T() {
    static Tables t;
    return t;
}

// Piece match helpers.
static constexpr inline bool is_slider_bishop_queen(Piece p, Color c) {
    return (c == WHITE) ? (p == W_BISHOP || p == W_QUEEN) : (p == B_BISHOP || p == B_QUEEN);
}
static constexpr inline bool is_slider_rook_queen(Piece p, Color c) {
    return (c == WHITE) ? (p == W_ROOK || p == W_QUEEN) : (p == B_ROOK || p == B_QUEEN);
}
static constexpr inline bool is_knight(Piece p, Color c) {
    return (c == WHITE) ? (p == W_KNIGHT) : (p == B_KNIGHT);
}
static constexpr inline bool is_king(Piece p, Color c) {
    return (c == WHITE) ? (p == W_KING) : (p == B_KING);
}
static constexpr inline bool is_pawn(Piece p, Color c) {
    return (c == WHITE) ? (p == W_PAWN) : (p == B_PAWN);
}

// Returns a bitboard of squares containing byColor pieces that attack sq.
inline Bitboard attackers_to_bb(const Position& pos, int sq, Color byColor) {
    if ((unsigned)sq >= 64u)
        return 0ULL;

    Bitboard atk = 0ULL;

    const int f = file_of(sq);
    const int r = rank_of(sq);

    // Pawn attackers (reverse lookup).
    if (byColor == WHITE) {
        // White pawn attacks up; attacker must be at (f-1, r-1) or (f+1, r-1).
        if ((unsigned)(r - 1) < 8u) {
            if ((unsigned)(f - 1) < 8u) {
                int from = make_sq(f - 1, r - 1);
                if (pos.board[from] == W_PAWN)
                    atk |= bb_sq(from);
            }
            if ((unsigned)(f + 1) < 8u) {
                int from = make_sq(f + 1, r - 1);
                if (pos.board[from] == W_PAWN)
                    atk |= bb_sq(from);
            }
        }
    } else {
        // Black pawn attacks down; attacker must be at (f-1, r+1) or (f+1, r+1).
        if ((unsigned)(r + 1) < 8u) {
            if ((unsigned)(f - 1) < 8u) {
                int from = make_sq(f - 1, r + 1);
                if (pos.board[from] == B_PAWN)
                    atk |= bb_sq(from);
            }
            if ((unsigned)(f + 1) < 8u) {
                int from = make_sq(f + 1, r + 1);
                if (pos.board[from] == B_PAWN)
                    atk |= bb_sq(from);
            }
        }
    }

    // --- knights ---
    {
        Bitboard nmask = T().knight[sq];
        while (nmask) {
            int from = __builtin_ctzll(nmask);
            nmask &= nmask - 1;
            Piece p = pos.board[from];
            if (p != NO_PIECE && is_knight(p, byColor))
                atk |= bb_sq(from);
        }
    }

    // --- kings ---
    {
        Bitboard kmask = T().king[sq];
        while (kmask) {
            int from = __builtin_ctzll(kmask);
            kmask &= kmask - 1;
            Piece p = pos.board[from];
            if (p != NO_PIECE && is_king(p, byColor))
                atk |= bb_sq(from);
        }
    }

    // Sliders: step by file/rank deltas, stop at first blocker.
    auto scan_dir = [&](int df, int dr, bool diag) {
        int ff = f + df;
        int rr = r + dr;
        while ((unsigned)ff < 8u && (unsigned)rr < 8u) {
            int from = make_sq(ff, rr);
            Piece p = pos.board[from];
            if (p != NO_PIECE) {
                if (diag) {
                    if (is_slider_bishop_queen(p, byColor))
                        atk |= bb_sq(from);
                } else {
                    if (is_slider_rook_queen(p, byColor))
                        atk |= bb_sq(from);
                }
                break; // first blocker decides
            }
            ff += df;
            rr += dr;
        }
    };

    // Diagonals
    scan_dir(+1, +1, true);
    scan_dir(-1, +1, true);
    scan_dir(+1, -1, true);
    scan_dir(-1, -1, true);

    // Orthogonals
    scan_dir(+1, 0, false);
    scan_dir(-1, 0, false);
    scan_dir(0, +1, false);
    scan_dir(0, -1, false);

    return atk;
}

// Count attackers by popcount.
inline int attackers_to_count(const Position& pos, int sq, Color byColor) {
    Bitboard b = attackers_to_bb(pos, sq, byColor);
    return (int)__builtin_popcountll(b);
}

// -------------------------------------
// Fast boolean check (use attackers_to_bb underneath)
// -------------------------------------
inline bool is_square_attacked(const Position& pos, int sq, Color byColor) {
    return attackers_to_bb(pos, sq, byColor) != 0ULL;
}

// True if sideToCheck's king is attacked.
inline bool in_check(const Position& pos, Color sideToCheck) {
    int ksq = pos.king_square(sideToCheck);
    if (ksq < 0)
        return false;
    return is_square_attacked(pos, ksq, flip(sideToCheck));
}

} // namespace attacks
