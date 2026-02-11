#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

#include "types.h"
#include "Position.h"

namespace eval {

// ======================================================
// Piece values (MG / EG)
// ======================================================
static constexpr int MG_VAL[7] = { 0,100,320,330,500,900,0 };
static constexpr int EG_VAL[7] = { 0,120,300,320,520,900,0 };

// 给 Search.h 用的接口：mvv-lva/排序会调用
inline int mg_value(PieceType pt) {
    if (pt < PAWN || pt > KING) return 0;
    return MG_VAL[(int)pt];
}

// ======================================================
// PST (A1=0 .. H8=63)
// ======================================================
static constexpr int PST_P_MG[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10,-20,-20, 10, 10,  5,
     5, -5,-10,  0,  0,-10, -5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5,  5, 10, 25, 25, 10,  5,  5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
     0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_N_MG[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50
};

static constexpr int PST_B_MG[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10,-10,-10,-10,-10,-20
};

static constexpr int PST_R_MG[64] = {
     0,  0,  5, 10, 10,  5,  0,  0,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static constexpr int PST_Q_MG[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20
};

static constexpr int PST_K_MG[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20
};

static constexpr int PST_K_EG[64] = {
   -50,-30,-30,-30,-30,-30,-30,-50,
   -30,-10,  0,  0,  0,  0,-10,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,-10,  0,  0,  0,  0,-10,-30,
   -50,-30,-30,-30,-30,-30,-30,-50
};

// mirror square for black
inline int mirror_sq(int sq) { return sq ^ 56; }

// ======================================================
// Fast utilities
// ======================================================
inline int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
inline Color flip_color(Color c) { return (c == WHITE) ? BLACK : WHITE; }

inline bool pawn_on(const Position& pos, int sq, Color c) {
    return pos.board[sq] == make_piece(c, PAWN);
}

inline bool center_opening(const Position& pos) {
    bool eMoved = !pawn_on(pos, E2, WHITE) || !pawn_on(pos, E7, BLACK);
    bool dMoved = !pawn_on(pos, D2, WHITE) || !pawn_on(pos, D7, BLACK);
    return eMoved || dMoved;
}

// ======================================================
// Weights
// ======================================================
static constexpr int BISHOP_PAIR_BONUS_MG = 25;
static constexpr int BISHOP_PAIR_BONUS_EG = 35;

// Rook activity
static constexpr int ROOK_OPEN_FILE_BONUS_MG     = 25;
static constexpr int ROOK_SEMIOPEN_FILE_BONUS_MG = 14;
static constexpr int ROOK_ON_7TH_BONUS_MG        = 18;
static constexpr int ROOK_CONNECTED_BONUS_MG     = 12;

// Pawn structure
static constexpr int DOUBLED_PAWN_PENALTY  = 10;
static constexpr int ISOLATED_PAWN_PENALTY = 12;
static constexpr int CONNECTED_PAWN_BONUS  = 6;

// Passed pawn
static constexpr int PASSED_PAWN_BONUS_EG[8] = { 0, 6, 10, 18, 30, 45, 70, 0 };

// King shield (cheap)
static constexpr int KING_SHIELD_BONUS_MG = 8;

// Center / space
static constexpr int CENTER_PAWN_BONUS_MG = 6;
static constexpr int SPACE_BONUS_MG       = 1;

// ✅ “鳕鱼味”但超便宜：king tropism（只算距离，不算攻击格）
// dist = manhattan distance to enemy king (0..14)
static constexpr int TROP_Q_MG = 6;
static constexpr int TROP_R_MG = 4;
static constexpr int TROP_B_MG = 3;
static constexpr int TROP_N_MG = 3;

// 开放王翼/王线惩罚（只用 pawnFile，几乎免费）
static constexpr int KING_OPEN_FILE_PEN_MG     = 18;
static constexpr int KING_SEMIOPEN_FILE_PEN_MG = 10;

// ✅ 更“鳕鱼味”的超轻王攻驱动：开线/王圈/兵风暴/先手
static constexpr int KING_LINE_Q_MG = 26;   // queen has clear line to king
static constexpr int KING_LINE_R_MG = 18;   // rook has clear line to king
static constexpr int KING_LINE_B_MG = 14;   // bishop has clear diag to king

static constexpr int KING_RING_N2_MG = 10;  // knight within manhattan<=2
static constexpr int KING_RING_N3_MG = 5;   // knight within manhattan<=3
static constexpr int KING_RING_Q3_MG = 4;   // queen within manhattan<=3
static constexpr int KING_RING_B3_MG = 3;   // bishop within manhattan<=3
static constexpr int KING_RING_R3_MG = 3;   // rook  within manhattan<=3

static constexpr int PAWN_STORM_STEP_MG = 4; // per extra rank advanced toward king
static constexpr int INITIATIVE_BONUS_MG = 6; // side-to-move bonus in MG-ish

// ======================================================
// Phase (0..256)
// ======================================================
inline int game_phase_256(const Position& pos) {
    int phase = 0;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = pos.board[sq];
        if (p == NO_PIECE) continue;
        switch (type_of(p)) {
            case KNIGHT: phase += 1; break;
            case BISHOP: phase += 1; break;
            case ROOK:   phase += 2; break;
            case QUEEN:  phase += 4; break;
            default: break;
        }
    }
    phase = clamp(phase, 0, 24);
    return (phase * 256) / 24;
}

// ======================================================
// Between checks
// ======================================================
inline bool clear_between_rank(const Position& pos, int a, int b) {
    int ra = rank_of(a), rb = rank_of(b);
    if (ra != rb) return false;
    int fa = file_of(a), fb = file_of(b);
    int step = (fa < fb) ? 1 : -1;
    for (int f = fa + step; f != fb; f += step) {
        int sq = make_sq(f, ra);
        if (pos.board[sq] != NO_PIECE) return false;
    }
    return true;
}

inline bool clear_between_file(const Position& pos, int a, int b) {
    int fa = file_of(a), fb = file_of(b);
    if (fa != fb) return false;
    int ra = rank_of(a), rb = rank_of(b);
    int step = (ra < rb) ? 1 : -1;
    for (int r = ra + step; r != rb; r += step) {
        int sq = make_sq(fa, r);
        if (pos.board[sq] != NO_PIECE) return false;
    }
    return true;
}

// ✅ diag between (for bishop/queen king-line pressure)
inline bool clear_between_diag(const Position& pos, int a, int b) {
    int fa = file_of(a), fb = file_of(b);
    int ra = rank_of(a), rb = rank_of(b);
    int df = fb - fa, dr = rb - ra;
    if (df == 0 || std::abs(df) != std::abs(dr)) return false;

    int stepF = (df > 0) ? 1 : -1;
    int stepR = (dr > 0) ? 1 : -1;

    for (int f = fa + stepF, r = ra + stepR; f != fb; f += stepF, r += stepR) {
        int sq = make_sq(f, r);
        if (pos.board[sq] != NO_PIECE) return false;
    }
    return true;
}

// ======================================================
// Opening development bonus (cheap, keep)
// ======================================================
inline int development_bonus_mg(const Position& pos) {
    int bonus = 0;
    auto piece_at = [&](int sq) { return pos.board[sq]; };

    if (piece_at(B1) == W_KNIGHT) bonus -= 4;
    if (piece_at(G1) == W_KNIGHT) bonus -= 4;
    if (piece_at(B8) == B_KNIGHT) bonus += 4;
    if (piece_at(G8) == B_KNIGHT) bonus += 4;

    if (piece_at(C1) == W_BISHOP) bonus -= 8;
    if (piece_at(F1) == W_BISHOP) bonus -= 8;
    if (piece_at(C8) == B_BISHOP) bonus += 8;
    if (piece_at(F8) == B_BISHOP) bonus += 8;

    if (piece_at(D1) != W_QUEEN) bonus -= 6;
    if (piece_at(D8) != B_QUEEN) bonus += 6;

    return bonus;
}

// ======================================================
// Core evaluate()
// ======================================================
inline int evaluate(const Position& pos) {
    int pawnFile[2][8] = {};
    unsigned char pawnRanks[2][8] = {};

    int rookSquares[2][2] = {{-1,-1},{-1,-1}};
    int rookNum[2] = {0,0};

    int bishopCount[2] = {0,0};
    int kingSq[2] = {-1, -1};

    // store attacker piece squares for tropism (max counts are small)
    int qSq[2][1] = {{-1},{-1}};
    int qNum[2] = {0,0};

    int nSq[2][2] = {{-1,-1},{-1,-1}};
    int nNum[2] = {0,0};

    int bSq[2][2] = {{-1,-1},{-1,-1}};
    int bNum[2] = {0,0};

    int rSq[2][2] = {{-1,-1},{-1,-1}};
    int rNum2[2] = {0,0};

    int mg = 0;
    int eg = 0;

    for (int sq = 0; sq < 64; sq++) {
        Piece p = pos.board[sq];
        if (p == NO_PIECE) continue;

        Color c = color_of(p);
        PieceType pt = type_of(p);
        int sign = (c == WHITE) ? +1 : -1;

        int s = (c == WHITE) ? sq : mirror_sq(sq);

        mg += sign * MG_VAL[(int)pt];
        eg += sign * EG_VAL[(int)pt];

        switch (pt) {
            case PAWN:   mg += sign * PST_P_MG[s]; break;
            case KNIGHT: mg += sign * PST_N_MG[s]; break;
            case BISHOP: mg += sign * PST_B_MG[s]; break;
            case ROOK:   mg += sign * PST_R_MG[s]; break;
            case QUEEN:  mg += sign * PST_Q_MG[s]; break;
            case KING:
                mg += sign * PST_K_MG[s];
                eg += sign * PST_K_EG[s];
                kingSq[c] = sq;
                break;
            default: break;
        }

        if (pt == PAWN) {
            int f = file_of(sq);
            int r = rank_of(sq);
            pawnFile[c][f]++;
            pawnRanks[c][f] |= (unsigned char)(1u << r);

            if (sq == D4 || sq == E4 || sq == D5 || sq == E5) mg += sign * CENTER_PAWN_BONUS_MG;
            if ((c == WHITE && r >= 4) || (c == BLACK && r <= 3)) mg += sign * SPACE_BONUS_MG;
        }

        if (pt == ROOK) {
            if (rookNum[c] < 2) rookSquares[c][rookNum[c]] = sq;
            rookNum[c]++;
            if (rNum2[c] < 2) rSq[c][rNum2[c]++] = sq;
        }

        if (pt == BISHOP) {
            bishopCount[c]++;
            if (bNum[c] < 2) bSq[c][bNum[c]++] = sq;
        }

        if (pt == KNIGHT) {
            if (nNum[c] < 2) nSq[c][nNum[c]++] = sq;
        }

        if (pt == QUEEN) {
            if (qNum[c] < 1) qSq[c][qNum[c]++] = sq;
        }
    }

    // --- Bishop pair ---
    if (bishopCount[WHITE] >= 2) { mg += BISHOP_PAIR_BONUS_MG; eg += BISHOP_PAIR_BONUS_EG; }
    if (bishopCount[BLACK] >= 2) { mg -= BISHOP_PAIR_BONUS_MG; eg -= BISHOP_PAIR_BONUS_EG; }

    // --- King shield (cheap) ---
    auto king_shield = [&](Color c) -> int {
        int k = kingSq[c];
        if (k < 0) return 0;

        int f = file_of(k);
        int r = rank_of(k);

        bool castledLike = (f >= 5 || f <= 2);
        if (!castledLike) return 0;

        int bonus = 0;
        int dr = (c == WHITE) ? +1 : -1;

        for (int layer = 1; layer <= 2; layer++) {
            int rr = r + dr * layer;
            if (rr < 0 || rr > 7) continue;

            int layerBonus = (layer == 1) ? KING_SHIELD_BONUS_MG : (KING_SHIELD_BONUS_MG / 2);
            for (int df = -1; df <= 1; df++) {
                int ff = f + df;
                if (ff < 0 || ff > 7) continue;
                int sqq = make_sq(ff, rr);
                Piece pp = pos.board[sqq];
                if (pp != NO_PIECE && color_of(pp) == c && type_of(pp) == PAWN)
                    bonus += layerBonus;
            }
        }
        return bonus;
    };
    mg += king_shield(WHITE);
    mg -= king_shield(BLACK);

    // --- Rook activity ---
    auto rook_activity = [&](Color c) {
        int sign = (c == WHITE) ? +1 : -1;
        Color opp = flip_color(c);

        for (int i = 0; i < 2; i++) {
            int sq = rookSquares[c][i];
            if (sq < 0) continue;

            int f = file_of(sq);
            bool ownPawnOnFile = pawnFile[c][f] > 0;
            bool oppPawnOnFile = pawnFile[opp][f] > 0;

            if (!ownPawnOnFile && !oppPawnOnFile) mg += sign * ROOK_OPEN_FILE_BONUS_MG;
            else if (!ownPawnOnFile && oppPawnOnFile) mg += sign * ROOK_SEMIOPEN_FILE_BONUS_MG;

            int r = rank_of(sq);
            if (c == WHITE && r == 6) mg += sign * ROOK_ON_7TH_BONUS_MG;
            if (c == BLACK && r == 1) mg += sign * ROOK_ON_7TH_BONUS_MG;
        }

        if (rookSquares[c][0] >= 0 && rookSquares[c][1] >= 0) {
            int a = rookSquares[c][0];
            int b = rookSquares[c][1];
            bool connected = false;

            if (rank_of(a) == rank_of(b)) connected = clear_between_rank(pos, a, b);
            else if (file_of(a) == file_of(b)) connected = clear_between_file(pos, a, b);

            if (connected) mg += sign * ROOK_CONNECTED_BONUS_MG;
        }
    };
    rook_activity(WHITE);
    rook_activity(BLACK);

    // --- Pawn structure + passed pawns ---
    auto pawn_structure = [&](Color c) {
        int sign = (c == WHITE) ? +1 : -1;
        Color opp = flip_color(c);

        for (int f = 0; f < 8; f++) {
            int n = pawnFile[c][f];
            if (n >= 2) {
                int pen = DOUBLED_PAWN_PENALTY * (n - 1);
                mg -= sign * pen;
                eg -= sign * pen;
            }
        }

        for (int f = 0; f < 8; f++) {
            unsigned int mask = (unsigned int)pawnRanks[c][f] & 0xFFu;
            while (mask) {
#if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                int r = (int)idx;
#else
                int r = __builtin_ctz(mask);
#endif
                mask &= (mask - 1);

                bool leftFile  = (f > 0) && pawnFile[c][f - 1] > 0;
                bool rightFile = (f < 7) && pawnFile[c][f + 1] > 0;
                if (!leftFile && !rightFile) {
                    mg -= sign * ISOLATED_PAWN_PENALTY;
                    eg -= sign * ISOLATED_PAWN_PENALTY;
                }

                unsigned char nearMask = 0;
                if (r > 0) nearMask |= (unsigned char)(1u << (r - 1));
                nearMask |= (unsigned char)(1u << r);
                if (r < 7) nearMask |= (unsigned char)(1u << (r + 1));

                bool connected = false;
                if (f > 0 && (pawnRanks[c][f - 1] & nearMask)) connected = true;
                if (f < 7 && (pawnRanks[c][f + 1] & nearMask)) connected = true;

                if (connected) {
                    mg += sign * CONNECTED_PAWN_BONUS;
                    eg += sign * CONNECTED_PAWN_BONUS;
                }

                unsigned char aheadMask;
                if (c == WHITE) {
                    aheadMask = (r >= 7) ? 0 : (unsigned char)(0xFFu & ~((1u << (r + 1)) - 1u));
                } else {
                    aheadMask = (r <= 0) ? 0 : (unsigned char)((1u << r) - 1u);
                }

                bool passed = true;
                for (int df = -1; df <= 1; df++) {
                    int ff = f + df;
                    if ((unsigned)ff > 7u) continue;
                    if (pawnRanks[opp][ff] & aheadMask) { passed = false; break; }
                }

                if (passed) {
                    int pr = (c == WHITE) ? r : (7 - r);
                    pr = clamp(pr, 0, 7);
                    int egBonus = PASSED_PAWN_BONUS_EG[pr];
                    eg += sign * egBonus;
                    mg += sign * (egBonus / 4);
                }
            }
        }
    };
    pawn_structure(WHITE);
    pawn_structure(BLACK);

    // ======================================================
    // ✅ “鳕鱼味”但不掉速：Tropism + King-file openness + King pressure
    // ======================================================
    auto trop = [&](Color attacker) -> int {
        Color defender = flip_color(attacker);
        int k = kingSq[defender];
        if (k < 0) return 0;

        int kf = file_of(k), kr = rank_of(k);

        auto manhattan = [&](int sq) -> int {
            return std::abs(file_of(sq) - kf) + std::abs(rank_of(sq) - kr); // 0..14
        };

        int bonus = 0;

        // queen
        if (qSq[attacker][0] >= 0) {
            int d = manhattan(qSq[attacker][0]);
            int v = (14 - d);
            if (v > 0) bonus += v * TROP_Q_MG;
        }

        // rooks
        for (int i = 0; i < rNum2[attacker]; i++) {
            int d = manhattan(rSq[attacker][i]);
            int v = (14 - d);
            if (v > 0) bonus += v * TROP_R_MG;
        }

        // bishops
        for (int i = 0; i < bNum[attacker]; i++) {
            int d = manhattan(bSq[attacker][i]);
            int v = (14 - d);
            if (v > 0) bonus += v * TROP_B_MG;
        }

        // knights
        for (int i = 0; i < nNum[attacker]; i++) {
            int d = manhattan(nSq[attacker][i]);
            int v = (14 - d);
            if (v > 0) bonus += v * TROP_N_MG;
        }

        // open / semi-open king file (super cheap)
        int kfile = kf;
        bool defPawn = pawnFile[defender][kfile] > 0;
        bool atkPawn = pawnFile[attacker][kfile] > 0;

        // 只有中局才用（防止残局乱扣）
        if (defPawn == false) {
            bonus += atkPawn ? KING_SEMIOPEN_FILE_PEN_MG : KING_OPEN_FILE_PEN_MG;
        }

        return bonus;
    };

    // pawn rank helpers (for pawn storm)
    auto top_bit = [&](unsigned char m) -> int { // highest rank bit (0..7), -1 if empty
        for (int r = 7; r >= 0; --r) if (m & (1u << r)) return r;
        return -1;
    };
    auto low_bit = [&](unsigned char m) -> int { // lowest rank bit (0..7), -1 if empty
        for (int r = 0; r <= 7; ++r) if (m & (1u << r)) return r;
        return -1;
    };

    auto king_pressure = [&](Color attacker) -> int {
        Color defender = flip_color(attacker);
        int k = kingSq[defender];
        if (k < 0) return 0;

        int kf = file_of(k), kr = rank_of(k);

        auto manhattan_to_king = [&](int sq) -> int {
            return std::abs(file_of(sq) - kf) + std::abs(rank_of(sq) - kr);
        };

        int bonus = 0;

        // ---- 1) Line-of-sight pressure (encourages sacrifices to open lines) ----
        if (qSq[attacker][0] >= 0) {
            int qs = qSq[attacker][0];
            if (file_of(qs) == kf && clear_between_file(pos, qs, k)) bonus += KING_LINE_Q_MG;
            if (rank_of(qs) == kr && clear_between_rank(pos, qs, k)) bonus += KING_LINE_Q_MG;
            if (std::abs(file_of(qs) - kf) == std::abs(rank_of(qs) - kr) && clear_between_diag(pos, qs, k))
                bonus += KING_LINE_Q_MG;
        }

        for (int i = 0; i < rNum2[attacker]; i++) {
            int rs = rSq[attacker][i];
            if (rs < 0) continue;
            if (file_of(rs) == kf && clear_between_file(pos, rs, k)) bonus += KING_LINE_R_MG;
            if (rank_of(rs) == kr && clear_between_rank(pos, rs, k)) bonus += KING_LINE_R_MG;
        }

        for (int i = 0; i < bNum[attacker]; i++) {
            int bs = bSq[attacker][i];
            if (bs < 0) continue;
            if (std::abs(file_of(bs) - kf) == std::abs(rank_of(bs) - kr) && clear_between_diag(pos, bs, k))
                bonus += KING_LINE_B_MG;
        }

        // ---- 2) King ring proximity (very cheap “attack potential”) ----
        for (int i = 0; i < nNum[attacker]; i++) {
            int ds = manhattan_to_king(nSq[attacker][i]);
            if (ds <= 2) bonus += KING_RING_N2_MG;
            else if (ds <= 3) bonus += KING_RING_N3_MG;
        }

        if (qSq[attacker][0] >= 0) {
            int ds = manhattan_to_king(qSq[attacker][0]);
            if (ds <= 3) bonus += KING_RING_Q3_MG;
        }

        for (int i = 0; i < bNum[attacker]; i++) {
            int ds = manhattan_to_king(bSq[attacker][i]);
            if (ds <= 3) bonus += KING_RING_B3_MG;
        }

        for (int i = 0; i < rNum2[attacker]; i++) {
            int ds = manhattan_to_king(rSq[attacker][i]);
            if (ds <= 3) bonus += KING_RING_R3_MG;
        }

        // ---- 3) Pawn storm toward a castled king (cheap, uses pawnRanks) ----
        bool kingSide  = (kf >= 5);
        bool queenSide = (kf <= 2);

        if (kingSide || queenSide) {
            int f0 = kingSide ? 5 : 0; // f/g/h => 5..7, a/b/c => 0..2
            int f1 = kingSide ? 7 : 2;

            for (int f = f0; f <= f1; f++) {
                unsigned char m = pawnRanks[attacker][f];
                int r = (attacker == WHITE) ? top_bit(m) : low_bit(m);
                if (r < 0) continue;

                int adv = (attacker == WHITE) ? r : (7 - r); // 0..7
                if (adv >= 3) bonus += (adv - 2) * PAWN_STORM_STEP_MG;
            }
        }

        return bonus;
    };

    int phase = game_phase_256(pos); // 0..256
    if (phase > 110) { // 中局才上“鳕鱼味”
        mg += trop(WHITE);
        mg -= trop(BLACK);

        mg += king_pressure(WHITE);
        mg -= king_pressure(BLACK);

        // 先手/主动性：减少保守重复
        mg += (pos.side == WHITE ? +INITIATIVE_BONUS_MG : -INITIATIVE_BONUS_MG);
    }

    // ======================================================
    // Tapered eval blend
    // ======================================================
    int score = (mg * phase + eg * (256 - phase)) / 256;

    // Opening dev bonus only in MG-ish phase
    if (phase > 160) score += development_bonus_mg(pos);

    return (pos.side == WHITE) ? score : -score;
}

} // namespace eval
