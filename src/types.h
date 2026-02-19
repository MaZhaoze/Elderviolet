#pragma once
#include <cstdint>

// Core chess types and move encoding.

// Squares 0..63 with a1 = 0 and h8 = 63.
enum Square : int {
    A1 = 0,
    B1,
    C1,
    D1,
    E1,
    F1,
    G1,
    H1,
    A2,
    B2,
    C2,
    D2,
    E2,
    F2,
    G2,
    H2,
    A3,
    B3,
    C3,
    D3,
    E3,
    F3,
    G3,
    H3,
    A4,
    B4,
    C4,
    D4,
    E4,
    F4,
    G4,
    H4,
    A5,
    B5,
    C5,
    D5,
    E5,
    F5,
    G5,
    H5,
    A6,
    B6,
    C6,
    D6,
    E6,
    F6,
    G6,
    H6,
    A7,
    B7,
    C7,
    D7,
    E7,
    F7,
    G7,
    H7,
    A8,
    B8,
    C8,
    D8,
    E8,
    F8,
    G8,
    H8
};

// 0-based file/rank helpers.
inline int file_of(int sq) { return sq & 7; }
inline int rank_of(int sq) { return sq >> 3; }
inline int make_sq(int f, int r) { return (r << 3) | f; }

// Side to move / piece color.
enum Color : int { WHITE = 0, BLACK = 1 };
inline Color operator~(Color c) {
    return (c == WHITE) ? BLACK : WHITE;
}

// Piece type codes (NONE = 0).
enum PieceType : int { NONE = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

// Encoded piece: 0 = NO_PIECE, 1..6 white, 9..14 black.
enum Piece : int {
    NO_PIECE = 0,

    W_PAWN = 1,
    W_KNIGHT = 2,
    W_BISHOP = 3,
    W_ROOK = 4,
    W_QUEEN = 5,
    W_KING = 6,
    B_PAWN = 9,
    B_KNIGHT = 10,
    B_BISHOP = 11,
    B_ROOK = 12,
    B_QUEEN = 13,
    B_KING = 14
};

inline Color color_of(Piece p) {
    // NO_PIECE has no color; return WHITE by convention for callers that expect a value.
    if (p == NO_PIECE)
        return WHITE;
    return (p >= B_PAWN) ? BLACK : WHITE;
}

inline PieceType type_of(Piece p) {
    if (p == NO_PIECE)
        return NONE;
    int v = int(p) & 7; // 1..6
    return PieceType(v);
}

inline bool same_color(Piece p, Color c) {
    return p != NO_PIECE && color_of(p) == c;
}
inline bool enemy_color(Piece p, Color c) {
    return p != NO_PIECE && color_of(p) != c;
}

inline Piece make_piece(Color c, PieceType pt) {
    return (c == WHITE) ? Piece(int(pt)) : Piece(int(pt) + 8);
}

// Move encoding (32-bit):
//  0..5   from (0..63)
//  6..11  to   (0..63)
// 12..15  flags (4 bits)
// 16..18  promo (3 bits): 0 none, 1=N, 2=B, 3=R, 4=Q
using Move = uint32_t;

// Bit flags applied to a move.
enum MoveFlag : int { MF_NONE = 0, MF_CAPTURE = 1 << 0, MF_EP = 1 << 1, MF_CASTLE = 1 << 2, MF_PROMO = 1 << 3 };

// `flags` goes before `promo` so bit packing stays stable across the codebase.
inline Move make_move(int from, int to, int flags = 0, int promo = 0) {
    return Move((from & 63) | ((to & 63) << 6) | ((flags & 15) << 12) | ((promo & 7) << 16));
}

inline int from_sq(Move m) {
    return int(m & 63);
}
inline int to_sq(Move m) {
    return int((m >> 6) & 63);
}
inline int flags_of(Move m) {
    return int((m >> 12) & 15);
}
inline int promo_of(Move m) {
    return int((m >> 16) & 7);
}
