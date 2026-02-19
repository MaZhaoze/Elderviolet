#pragma once
#include <string>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <cstdint>

#include "types.h"
#include "ZobristTables.h"

// Position representation and move make/undo with incremental Zobrist.

// Castling rights bitmask.
enum CastlingRight : int {
    CR_NONE = 0,
    CR_WK = 1 << 0, // White king side
    CR_WQ = 1 << 1, // White queen side
    CR_BK = 1 << 2, // Black king side
    CR_BQ = 1 << 3  // Black queen side
};

// Snapshot of state needed to undo a move.
struct Undo {
    Piece moved = NO_PIECE;
    Piece captured = NO_PIECE;

    Color prevSide = WHITE;

    int prevCastling = CR_NONE;
    int prevEpSquare = -1;
    int prevHalfmove = 0;
    int prevFullmove = 1;

    uint64_t prevKey = 0; // zobrist before move

    // En passant capture square (if any).
    int epCapturedSq = -1;

    // Castling rook move (if any).
    int rookFrom = -1;
    int rookTo = -1;
};

// Board state. Squares are 0..63 (a1 = 0), zobKey is incremental.
struct Position {
    Piece board[64];
    Color side = WHITE;

    int castlingRights = CR_WK | CR_WQ | CR_BK | CR_BQ;
    int epSquare = -1; // 0..63 or -1
    int halfmoveClock = 0;
    int fullmoveNumber = 1;

    uint64_t zobKey = 0; // incremental Zobrist key

    Position() {
        clear();
        set_startpos();
    }

    // Basic helpers.
    // Reset to an empty position (no pieces, no rights).
    void clear() {
        for (int i = 0; i < 64; i++)
            board[i] = NO_PIECE;
        side = WHITE;
        castlingRights = CR_NONE;
        epSquare = -1;
        halfmoveClock = 0;
        fullmoveNumber = 1;
        zobKey = 0;
    }

    static inline Piece char_to_piece(char c) {
        switch (c) {
        case 'P':
            return W_PAWN;
        case 'N':
            return W_KNIGHT;
        case 'B':
            return W_BISHOP;
        case 'R':
            return W_ROOK;
        case 'Q':
            return W_QUEEN;
        case 'K':
            return W_KING;
        case 'p':
            return B_PAWN;
        case 'n':
            return B_KNIGHT;
        case 'b':
            return B_BISHOP;
        case 'r':
            return B_ROOK;
        case 'q':
            return B_QUEEN;
        case 'k':
            return B_KING;
        default:
            return NO_PIECE;
        }
    }

    static inline int algebraic_to_sq(const std::string& s) {
        // "e3" -> square index, or -1 if invalid.
        if (s.size() != 2)
            return -1;
        char f = s[0], r = s[1];
        if (f < 'a' || f > 'h')
            return -1;
        if (r < '1' || r > '8')
            return -1;
        return make_sq(f - 'a', r - '1');
    }

    static inline std::string sq_to_algebraic(int sq) {
        if (sq < 0 || sq >= 64)
            return "-";
        std::string s;
        s += char('a' + file_of(sq));
        s += char('1' + rank_of(sq));
        return s;
    }

    // Full Zobrist recompute; used at init or for debugging.
    inline void recompute_zobrist() {
        uint64_t k = 0;
        for (int sq = 0; sq < 64; sq++) {
            Piece p = board[sq];
            if (p == NO_PIECE)
                continue;
            int pi = (int)p;
            if ((unsigned)pi >= 16u)
                continue;
            k ^= g_zob.psq[pi][sq];
        }
        if (side == BLACK)
            k ^= g_zob.sideKey;
        k ^= g_zob.castleKey[castlingRights & 15];
        if (epSquare != -1)
            k ^= g_zob.epKey[file_of(epSquare) & 7];
        zobKey = k;
    }

    // Incremental Zobrist update after do_move.
    // Assumes board/side/castlingRights/epSquare already updated.
    inline void apply_zobrist_delta_after_move(const Undo& u, Move m) {
        uint64_t k = u.prevKey;

        // EP key: remove old, add new
        if (u.prevEpSquare != -1)
            k ^= g_zob.epKey[file_of(u.prevEpSquare) & 7];
        if (epSquare != -1)
            k ^= g_zob.epKey[file_of(epSquare) & 7];

        // Castling key: remove old, add new
        k ^= g_zob.castleKey[u.prevCastling & 15];
        k ^= g_zob.castleKey[castlingRights & 15];

        // Side toggle
        k ^= g_zob.sideKey;

        const int from = from_sq(m);
        const int to = to_sq(m);

        // moved piece off from-square
        {
            int pi = (int)u.moved;
            if ((unsigned)pi < 16u)
                k ^= g_zob.psq[pi][from];
        }

        // captured piece off board (normal capture on 'to', EP on u.epCapturedSq)
        if (flags_of(m) & MF_EP) {
            if (u.epCapturedSq != -1 && u.captured != NO_PIECE) {
                int pi = (int)u.captured;
                if ((unsigned)pi < 16u)
                    k ^= g_zob.psq[pi][u.epCapturedSq];
            }
        } else if (u.captured != NO_PIECE) {
            int pi = (int)u.captured;
            if ((unsigned)pi < 16u)
                k ^= g_zob.psq[pi][to];
        }

        // final piece on 'to' (after promotion/castle/normal)
        {
            Piece finalP = board[to];
            int pi = (int)finalP;
            if ((unsigned)pi < 16u)
                k ^= g_zob.psq[pi][to];
        }

        // castling rook move
        if (flags_of(m) & MF_CASTLE) {
            if (u.rookFrom != -1 && u.rookTo != -1) {
                Piece rook = make_piece(u.prevSide, ROOK);
                int pi = (int)rook;
                if ((unsigned)pi < 16u) {
                    k ^= g_zob.psq[pi][u.rookFrom];
                    k ^= g_zob.psq[pi][u.rookTo];
                }
            }
        }

        zobKey = k;
    }

    // Standard initial position.
    void set_startpos() {
        clear();

        // White pieces
        board[A1] = W_ROOK;
        board[B1] = W_KNIGHT;
        board[C1] = W_BISHOP;
        board[D1] = W_QUEEN;
        board[E1] = W_KING;
        board[F1] = W_BISHOP;
        board[G1] = W_KNIGHT;
        board[H1] = W_ROOK;
        for (int f = 0; f < 8; f++)
            board[make_sq(f, 1)] = W_PAWN;

        // Black pieces
        board[A8] = B_ROOK;
        board[B8] = B_KNIGHT;
        board[C8] = B_BISHOP;
        board[D8] = B_QUEEN;
        board[E8] = B_KING;
        board[F8] = B_BISHOP;
        board[G8] = B_KNIGHT;
        board[H8] = B_ROOK;
        for (int f = 0; f < 8; f++)
            board[make_sq(f, 6)] = B_PAWN;

        side = WHITE;
        castlingRights = CR_WK | CR_WQ | CR_BK | CR_BQ;
        epSquare = -1;
        halfmoveClock = 0;
        fullmoveNumber = 1;

        recompute_zobrist();
    }

    // FEN parser (full):
    // 1 board / 2 side / 3 castling / 4 ep / 5 halfmove / 6 fullmove
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
            if (sq >= 0 && sq < 64)
                board[sq] = p;
            sq++;
        }

        // 2) side
        side = (sidePart == "b") ? BLACK : WHITE;

        // 3) castling
        castlingRights = CR_NONE;
        if (!castlingPart.empty() && castlingPart != "-") {
            for (char c : castlingPart) {
                if (c == 'K')
                    castlingRights |= CR_WK;
                else if (c == 'Q')
                    castlingRights |= CR_WQ;
                else if (c == 'k')
                    castlingRights |= CR_BK;
                else if (c == 'q')
                    castlingRights |= CR_BQ;
            }
        }

        // 4) ep
        if (epPart == "-" || epPart.empty())
            epSquare = -1;
        else
            epSquare = algebraic_to_sq(epPart);

        // defaults if missing
        if (halfmoveClock < 0)
            halfmoveClock = 0;
        if (fullmoveNumber <= 0)
            fullmoveNumber = 1;

        recompute_zobrist();
    }

    // Castling rights update helpers.
    inline void remove_castling_for_king(Color c) {
        if (c == WHITE)
            castlingRights &= ~(CR_WK | CR_WQ);
        else
            castlingRights &= ~(CR_BK | CR_BQ);
    }

    inline void remove_castling_for_rook_square(int sq) {
        // if a rook moves from or is captured on its original square
        if (sq == H1)
            castlingRights &= ~CR_WK;
        else if (sq == A1)
            castlingRights &= ~CR_WQ;
        else if (sq == H8)
            castlingRights &= ~CR_BK;
        else if (sq == A8)
            castlingRights &= ~CR_BQ;
    }

    // Make/unmake a move. Supports normal, capture, promotion, en passant, castling.
    Undo do_move(Move m) {
        Undo u;
        u.prevSide = side; // restore side exactly on undo
        u.prevCastling = castlingRights;
        u.prevEpSquare = epSquare;
        u.prevHalfmove = halfmoveClock;
        u.prevFullmove = fullmoveNumber;
        u.prevKey = zobKey; // base for fast undo and incremental key update

        const int from = from_sq(m);
        const int to = to_sq(m);

        u.moved = board[from];
        u.captured = board[to];

        Piece movedPiece = board[from];
        PieceType movedType = type_of(movedPiece);
        Color us = side;

        // reset EP by default
        epSquare = -1;

        // halfmove clock reset on pawn move or capture
        bool isCapture = (u.captured != NO_PIECE) || (flags_of(m) & MF_EP);
        if (movedType == PAWN || isCapture)
            halfmoveClock = 0;
        else
            halfmoveClock++;

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
                    u.rookFrom = H1;
                    u.rookTo = F1;
                } else if (from == E1 && to == C1) { // O-O-O
                    u.rookFrom = A1;
                    u.rookTo = D1;
                }
            } else {
                if (from == E8 && to == G8) { // O-O
                    u.rookFrom = H8;
                    u.rookTo = F8;
                } else if (from == E8 && to == C8) { // O-O-O
                    u.rookFrom = A8;
                    u.rookTo = D8;
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
                if (promo == 1)
                    pt = KNIGHT;
                else if (promo == 2)
                    pt = BISHOP;
                else if (promo == 3)
                    pt = ROOK;
                else if (promo == 4)
                    pt = QUEEN;

                board[to] = make_piece(us, pt);
            }

            // set ep square if pawn double move
            if (movedType == PAWN) {
                int dr = rank_of(to) - rank_of(from);
                if (us == WHITE && dr == 2)
                    epSquare = from + 8;
                else if (us == BLACK && dr == -2)
                    epSquare = from - 8;
            }
        }

        // Toggle side
        side = ~side;

        // Fullmove increments after Black makes a move
        if (us == BLACK)
            fullmoveNumber++;

        apply_zobrist_delta_after_move(u, m);

        return u;
    }

    void undo_move(Move m, const Undo& u) {
        const int from = from_sq(m);
        const int to = to_sq(m);

        // restore state first
        castlingRights = u.prevCastling;
        epSquare = u.prevEpSquare;
        halfmoveClock = u.prevHalfmove;
        fullmoveNumber = u.prevFullmove;

        side = u.prevSide; // do not infer; use saved side

        // --- undo castling rook move ---
        if (u.rookFrom != -1 && u.rookTo != -1) {
            board[u.rookFrom] = board[u.rookTo];
            board[u.rookTo] = NO_PIECE;
        }

        // --- undo en passant ---
        if (u.epCapturedSq != -1) {
            board[from] = u.moved;
            board[to] = NO_PIECE; // EP target square was empty
            board[u.epCapturedSq] = u.captured;
            zobKey = u.prevKey; // fast restore key
            return;
        }

        // Normal undo (includes promo)
        board[from] = u.moved;
        board[to] = u.captured;

        zobKey = u.prevKey; // fast restore key (always correct)
    }

    // For debugging / sanity
    int king_square(Color c) const {
        Piece king = (c == WHITE) ? W_KING : B_KING;
        for (int i = 0; i < 64; i++)
            if (board[i] == king)
                return i;
        return -1;
    }
};
