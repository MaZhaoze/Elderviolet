#pragma once
#include <string>
#include <sstream>
#include <cctype>
#include <algorithm>

#include "types.h"

// =====================
// Castling rights bits
// =====================
enum CastlingRight : int {
    CR_NONE = 0,
    CR_WK = 1 << 0, // White king side
    CR_WQ = 1 << 1, // White queen side
    CR_BK = 1 << 2, // Black king side
    CR_BQ = 1 << 3  // Black queen side
};

// =====================
// Undo info for unmake move
// =====================
struct Undo {
    Piece moved = NO_PIECE;
    Piece captured = NO_PIECE;

    // ✅关键：保存走棋前轮到谁
    Color prevSide = WHITE;

    int prevCastling = CR_NONE;
    int prevEpSquare = -1;
    int prevHalfmove = 0;
    int prevFullmove = 1;

    // For en passant capture
    int epCapturedSq = -1;

    // For castling rook move
    int rookFrom = -1;
    int rookTo = -1;
};

// =====================
// Position
// =====================
struct Position {
    Piece board[64];
    Color side = WHITE;

    int castlingRights = CR_WK | CR_WQ | CR_BK | CR_BQ;
    int epSquare = -1;         // 0..63 or -1
    int halfmoveClock = 0;
    int fullmoveNumber = 1;

    Position() {
        clear();
        set_startpos();
    }

    // ---------------------
    // basic helpers
    // ---------------------
    void clear() {
        for (int i = 0; i < 64; i++) board[i] = NO_PIECE;
        side = WHITE;
        castlingRights = CR_NONE;
        epSquare = -1;
        halfmoveClock = 0;
        fullmoveNumber = 1;
    }

    static inline Piece char_to_piece(char c) {
        switch (c) {
            case 'P': return W_PAWN;
            case 'N': return W_KNIGHT;
            case 'B': return W_BISHOP;
            case 'R': return W_ROOK;
            case 'Q': return W_QUEEN;
            case 'K': return W_KING;
            case 'p': return B_PAWN;
            case 'n': return B_KNIGHT;
            case 'b': return B_BISHOP;
            case 'r': return B_ROOK;
            case 'q': return B_QUEEN;
            case 'k': return B_KING;
            default:  return NO_PIECE;
        }
    }

    static inline int algebraic_to_sq(const std::string& s) {
        // "e3" -> square index
        if (s.size() != 2) return -1;
        char f = s[0], r = s[1];
        if (f < 'a' || f > 'h') return -1;
        if (r < '1' || r > '8') return -1;
        return make_sq(f - 'a', r - '1');
    }

    static inline std::string sq_to_algebraic(int sq) {
        if (sq < 0 || sq >= 64) return "-";
        std::string s;
        s += char('a' + file_of(sq));
        s += char('1' + rank_of(sq));
        return s;
    }

    // ---------------------
    // startpos
    // ---------------------
    void set_startpos() {
        clear();

        // White pieces
        board[A1]=W_ROOK;   board[B1]=W_KNIGHT; board[C1]=W_BISHOP; board[D1]=W_QUEEN;
        board[E1]=W_KING;   board[F1]=W_BISHOP; board[G1]=W_KNIGHT; board[H1]=W_ROOK;
        for (int f = 0; f < 8; f++) board[make_sq(f,1)] = W_PAWN;

        // Black pieces
        board[A8]=B_ROOK;   board[B8]=B_KNIGHT; board[C8]=B_BISHOP; board[D8]=B_QUEEN;
        board[E8]=B_KING;   board[F8]=B_BISHOP; board[G8]=B_KNIGHT; board[H8]=B_ROOK;
        for (int f = 0; f < 8; f++) board[make_sq(f,6)] = B_PAWN;

        side = WHITE;
        castlingRights = CR_WK | CR_WQ | CR_BK | CR_BQ;
        epSquare = -1;
        halfmoveClock = 0;
        fullmoveNumber = 1;
    }

    // ---------------------
    // FEN parser (full)
    // fen fields:
    // 1 board / 2 side / 3 castling / 4 ep / 5 halfmove / 6 fullmove
    // ---------------------
    void set_fen(const std::string& fen) {
        clear();

        std::istringstream iss(fen);
        std::string boardPart, sidePart, castlingPart, epPart;
        iss >> boardPart >> sidePart >> castlingPart >> epPart >> halfmoveClock >> fullmoveNumber;

        if (boardPart.empty()) {
            set_startpos();
            return;
        }

        // 1) board
        int sq = 56; // start from a8
        for (char c : boardPart) {
            if (c == '/') {
                sq -= 16;
                continue;
            }
            if (std::isdigit((unsigned char)c)) {
                sq += (c - '0');
                continue;
            }
            Piece p = char_to_piece(c);
            if (sq >= 0 && sq < 64) board[sq] = p;
            sq++;
        }

        // 2) side
        side = (sidePart == "b") ? BLACK : WHITE;

        // 3) castling
        castlingRights = CR_NONE;
        if (!castlingPart.empty() && castlingPart != "-") {
            for (char c : castlingPart) {
                if (c == 'K') castlingRights |= CR_WK;
                else if (c == 'Q') castlingRights |= CR_WQ;
                else if (c == 'k') castlingRights |= CR_BK;
                else if (c == 'q') castlingRights |= CR_BQ;
            }
        }

        // 4) ep
        if (epPart == "-" || epPart.empty()) epSquare = -1;
        else epSquare = algebraic_to_sq(epPart);

        // defaults if missing
        if (halfmoveClock < 0) halfmoveClock = 0;
        if (fullmoveNumber <= 0) fullmoveNumber = 1;
    }

    // ---------------------
    // Castling rights update helpers
    // ---------------------
    inline void remove_castling_for_king(Color c) {
        if (c == WHITE) castlingRights &= ~(CR_WK | CR_WQ);
        else castlingRights &= ~(CR_BK | CR_BQ);
    }

    inline void remove_castling_for_rook_square(int sq) {
        // if a rook moves from or is captured on its original square
        if (sq == H1) castlingRights &= ~CR_WK;
        else if (sq == A1) castlingRights &= ~CR_WQ;
        else if (sq == H8) castlingRights &= ~CR_BK;
        else if (sq == A8) castlingRights &= ~CR_BQ;
    }

    // ---------------------
    // do_move / undo_move
    // Supports:
    // - normal
    // - capture
    // - promotion
    // - en passant capture (MF_EP)
    // - castling (MF_CASTLE)
    // ---------------------
    Undo do_move(Move m) {
        Undo u;
        u.prevSide     = side;            // ✅关键：保存 side
        u.prevCastling = castlingRights;
        u.prevEpSquare = epSquare;
        u.prevHalfmove = halfmoveClock;
        u.prevFullmove = fullmoveNumber;

        const int from = from_sq(m);
        const int to   = to_sq(m);

        u.moved    = board[from];
        u.captured = board[to];

        Piece movedPiece = board[from];
        PieceType movedType = type_of(movedPiece);
        Color us = side;

        // reset EP by default
        epSquare = -1;

        // halfmove clock reset on pawn move or capture
        bool isCapture = (u.captured != NO_PIECE) || (flags_of(m) & MF_EP);
        if (movedType == PAWN || isCapture) halfmoveClock = 0;
        else halfmoveClock++;

        // --- Update castling rights based on moved piece
        if (movedType == KING) {
            remove_castling_for_king(us);
        } else if (movedType == ROOK) {
            remove_castling_for_rook_square(from);
        }

        // if captured rook on initial square -> remove enemy castling
        if (u.captured != NO_PIECE && type_of(u.captured) == ROOK) {
            remove_castling_for_rook_square(to);
        }

        // =====================
        // EN PASSANT
        // =====================
        if (flags_of(m) & MF_EP) {
            // captured pawn is behind "to"
            int capSq = (us == WHITE) ? (to - 8) : (to + 8);
            u.epCapturedSq = capSq;
            u.captured = board[capSq];

            board[capSq] = NO_PIECE;

            board[to] = movedPiece;
            board[from] = NO_PIECE;
        }
        // =====================
        // CASTLING
        // =====================
        else if (flags_of(m) & MF_CASTLE) {
            // move king
            board[to] = movedPiece;
            board[from] = NO_PIECE;

            // rook move record
            if (us == WHITE) {
                if (from == E1 && to == G1) { // O-O
                    u.rookFrom = H1; u.rookTo = F1;
                } else if (from == E1 && to == C1) { // O-O-O
                    u.rookFrom = A1; u.rookTo = D1;
                }
            } else {
                if (from == E8 && to == G8) { // O-O
                    u.rookFrom = H8; u.rookTo = F8;
                } else if (from == E8 && to == C8) { // O-O-O
                    u.rookFrom = A8; u.rookTo = D8;
                }
            }

            if (u.rookFrom != -1 && u.rookTo != -1) {
                board[u.rookTo] = board[u.rookFrom];
                board[u.rookFrom] = NO_PIECE;
            }

            // castling rights already removed by king move
        }
        // =====================
        // NORMAL / CAPTURE / PROMO
        // =====================
        else {
            board[to] = movedPiece;
            board[from] = NO_PIECE;

            // promotion
            int promo = promo_of(m);
            if (promo && movedType == PAWN) {
                PieceType pt = QUEEN;
                if (promo == 1) pt = KNIGHT;
                else if (promo == 2) pt = BISHOP;
                else if (promo == 3) pt = ROOK;
                else if (promo == 4) pt = QUEEN;

                board[to] = make_piece(us, pt);
            }

            // set ep square if pawn double move
            if (movedType == PAWN) {
                int dr = rank_of(to) - rank_of(from);
                if (us == WHITE && dr == 2) epSquare = from + 8;
                else if (us == BLACK && dr == -2) epSquare = from - 8;
            }
        }

        // Toggle side
        side = ~side;

        // Fullmove increments after Black makes a move
        if (us == BLACK) fullmoveNumber++;

        return u;
    }

    void undo_move(Move m, const Undo& u) {
        const int from = from_sq(m);
        const int to   = to_sq(m);

        // restore state first
        castlingRights = u.prevCastling;
        epSquare       = u.prevEpSquare;
        halfmoveClock  = u.prevHalfmove;
        fullmoveNumber = u.prevFullmove;

        // ✅关键：直接恢复 side（不要推断！）
        side = u.prevSide;

        // --- undo castling rook move ---
        // 用 rookFrom/rookTo 判断即可（稳定）
        if (u.rookFrom != -1 && u.rookTo != -1) {
            board[u.rookFrom] = board[u.rookTo];
            board[u.rookTo] = NO_PIECE;
        }

        // --- undo en passant ---
        if (u.epCapturedSq != -1) {
            board[from] = u.moved;
            board[to] = NO_PIECE; // EP target square was empty
            board[u.epCapturedSq] = u.captured;
            return;
        }

        // Normal undo (includes promo)
        board[from] = u.moved;
        board[to]   = u.captured;
    }

    // For debugging / sanity
    int king_square(Color c) const {
        Piece king = (c == WHITE) ? W_KING : B_KING;
        for (int i = 0; i < 64; i++)
            if (board[i] == king) return i;
        return -1;
    }
};
