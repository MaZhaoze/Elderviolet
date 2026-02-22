#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "MoveGeneration.h"
#include "PolyglotRandom.h"
#include "Position.h"
#include "types.h"

namespace book {

struct BookChoice {
    std::string uci;
    int weight = 1;
};

struct ProbeResult {
    Move bestMove = 0;
    std::vector<Move> pv;
};

inline int current_ply(const Position& pos) {
    return std::max(0, (pos.fullmoveNumber - 1) * 2 + (pos.side == BLACK ? 1 : 0));
}

inline std::string move_to_uci(Move m) {
    const int from = from_sq(m);
    const int to = to_sq(m);

    std::string s;
    s.reserve(5);
    s.push_back(char('a' + file_of(from)));
    s.push_back(char('1' + rank_of(from)));
    s.push_back(char('a' + file_of(to)));
    s.push_back(char('1' + rank_of(to)));

    const int promo = promo_of(m);
    if (promo) {
        char pc = 'q';
        if (promo == 1)
            pc = 'n';
        else if (promo == 2)
            pc = 'b';
        else if (promo == 3)
            pc = 'r';
        s.push_back(pc);
    }

    return s;
}

inline Move find_legal_by_uci(const Position& pos, const std::string& uci) {
    Position p = pos;
    std::vector<Move> moves;
    moves.reserve(256);
    movegen::generate_legal(p, moves);

    for (Move m : moves) {
        if (move_to_uci(m) == uci)
            return m;
    }
    return 0;
}

class OpeningBook {
  public:
    OpeningBook() { build(); }

    bool set_external_book_file(const std::string& path) { return load_external(path); }
    std::string external_book_file() const { return external_path_; }
    bool external_book_loaded() const { return external_loaded_; }

    ProbeResult probe(const Position& pos, int max_ply) const {
        ProbeResult out;
        if (max_ply <= 0)
            return out;
        if (current_ply(pos) >= max_ply)
            return out;

        // Policy override: vs 1.e4 prefer Sicilian most of the time.
        if (pos.zobKey == key_after_e4_) {
            Move sicilian = find_legal_by_uci(pos, "c7c5");
            if (sicilian && prefer_sicilian_70()) {
                out.bestMove = sicilian;
                out.pv = build_pv(pos, sicilian, max_ply);
                return out;
            }
        }

        Move m = pick_weighted(pos);
        if (!m)
            return out;
        out.bestMove = m;
        out.pv = build_pv(pos, m, max_ply);
        return out;
    }

  private:
    using WeightedList = std::vector<BookChoice>;
    struct PolyEntry {
        uint64_t key = 0;
        uint16_t polyMove = 0;
        uint16_t weight = 0;
    };

    struct WeightedMove {
        Move move = 0;
        int weight = 0;
    };

    std::unordered_map<uint64_t, WeightedList> table_;
    std::vector<PolyEntry> ext_;
    std::string external_path_;
    bool external_loaded_ = false;
    uint64_t key_after_e4_ = 0;

    static bool prefer_sicilian_70() {
        thread_local std::mt19937 rng((unsigned)std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 99);
        return dist(rng) < 70;
    }

    static uint64_t read_be_u64(const unsigned char* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++)
            v = (v << 8) | uint64_t(p[i]);
        return v;
    }
    static uint16_t read_be_u16(const unsigned char* p) {
        return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
    }

    bool load_external(const std::string& path) {
        ext_.clear();
        external_loaded_ = false;
        external_path_.clear();

        if (path.empty())
            return false;

        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;

        unsigned char rec[16];
        while (in.read(reinterpret_cast<char*>(rec), 16)) {
            PolyEntry e;
            e.key = read_be_u64(rec + 0);
            e.polyMove = read_be_u16(rec + 8);
            e.weight = read_be_u16(rec + 10);
            if (e.weight == 0)
                continue;
            ext_.push_back(e);
        }

        if (ext_.empty())
            return false;

        std::sort(ext_.begin(), ext_.end(), [](const PolyEntry& a, const PolyEntry& b) {
            if (a.key != b.key)
                return a.key < b.key;
            if (a.polyMove != b.polyMove)
                return a.polyMove < b.polyMove;
            return a.weight < b.weight;
        });

        external_path_ = path;
        external_loaded_ = true;
        return true;
    }

    Move poly_to_legal(const Position& pos, uint16_t pm) const {
        const int tf = (pm >> 0) & 7;
        const int tr = (pm >> 3) & 7;
        const int ff = (pm >> 6) & 7;
        const int fr = (pm >> 9) & 7;
        const int pp = (pm >> 12) & 7;

        const int from = make_sq(ff, fr);
        int to = make_sq(tf, tr);

        // Polyglot castling encoding uses king-to-rook squares.
        if (from == E1 && to == H1)
            to = G1;
        else if (from == E1 && to == A1)
            to = C1;
        else if (from == E8 && to == H8)
            to = G8;
        else if (from == E8 && to == A8)
            to = C8;

        int promo = 0;
        if (pp == 1)
            promo = 1; // N
        else if (pp == 2)
            promo = 2; // B
        else if (pp == 3)
            promo = 3; // R
        else if (pp == 4)
            promo = 4; // Q

        Position p = pos;
        std::vector<Move> moves;
        moves.reserve(256);
        movegen::generate_legal(p, moves);

        for (Move m : moves) {
            if (from_sq(m) == from && to_sq(m) == to && promo_of(m) == promo)
                return m;
        }
        return 0;
    }

    std::vector<WeightedMove> external_moves(const Position& pos) const {
        std::vector<WeightedMove> out;
        if (!external_loaded_ || ext_.empty())
            return out;

        const uint64_t polyKey = polyglot_key(pos);
        auto it = std::lower_bound(ext_.begin(), ext_.end(), polyKey, [](const PolyEntry& e, uint64_t k) { return e.key < k; });
        if (it == ext_.end() || it->key != polyKey)
            return out;

        for (; it != ext_.end() && it->key == polyKey; ++it) {
            Move m = poly_to_legal(pos, it->polyMove);
            if (!m)
                continue;

            bool merged = false;
            for (auto& wm : out) {
                if (wm.move == m) {
                    wm.weight += std::max(1, int(it->weight));
                    merged = true;
                    break;
                }
            }
            if (!merged)
                out.push_back(WeightedMove{m, std::max(1, int(it->weight))});
        }
        return out;
    }

    static int poly_piece_index(Piece p) {
        switch (p) {
        case B_PAWN:
            return 0;
        case W_PAWN:
            return 1;
        case B_KNIGHT:
            return 2;
        case W_KNIGHT:
            return 3;
        case B_BISHOP:
            return 4;
        case W_BISHOP:
            return 5;
        case B_ROOK:
            return 6;
        case W_ROOK:
            return 7;
        case B_QUEEN:
            return 8;
        case W_QUEEN:
            return 9;
        case B_KING:
            return 10;
        case W_KING:
            return 11;
        default:
            return -1;
        }
    }

    static uint64_t polyglot_key(const Position& pos) {
        uint64_t key = 0;

        for (int sq = 0; sq < 64; ++sq) {
            const Piece p = pos.board[sq];
            const int pi = poly_piece_index(p);
            if (pi >= 0)
                key ^= kPolyRandom[64 * pi + sq];
        }

        if (pos.castlingRights & CR_WK)
            key ^= kPolyRandom[768];
        if (pos.castlingRights & CR_WQ)
            key ^= kPolyRandom[769];
        if (pos.castlingRights & CR_BK)
            key ^= kPolyRandom[770];
        if (pos.castlingRights & CR_BQ)
            key ^= kPolyRandom[771];

        if (pos.epSquare >= 0) {
            const int f = file_of(pos.epSquare);
            const int r = rank_of(pos.epSquare);
            bool has_cap = false;
            if (pos.side == WHITE && r == 5) {
                if (f > 0 && pos.board[pos.epSquare - 1 - 8] == W_PAWN)
                    has_cap = true;
                if (f < 7 && pos.board[pos.epSquare + 1 - 8] == W_PAWN)
                    has_cap = true;
            } else if (pos.side == BLACK && r == 2) {
                if (f > 0 && pos.board[pos.epSquare - 1 + 8] == B_PAWN)
                    has_cap = true;
                if (f < 7 && pos.board[pos.epSquare + 1 + 8] == B_PAWN)
                    has_cap = true;
            }
            if (has_cap)
                key ^= kPolyRandom[772 + f];
        }

        if (pos.side == WHITE)
            key ^= kPolyRandom[780];

        return key;
    }

    static Move choose_weighted(const std::vector<WeightedMove>& choices, uint64_t mix) {
        int total_weight = 0;
        for (const auto& c : choices)
            total_weight += std::max(1, c.weight);
        if (total_weight <= 0)
            return 0;

        int pick = int(mix % uint64_t(total_weight));
        for (const auto& c : choices) {
            const int w = std::max(1, c.weight);
            if (pick < w)
                return c.move;
            pick -= w;
        }
        return 0;
    }

    Move pick_weighted_internal(const Position& pos) const {
        auto it = table_.find(pos.zobKey);
        if (it == table_.end() || it->second.empty())
            return 0;

        const auto& choices = it->second;
        int total_weight = 0;
        for (const auto& c : choices)
            total_weight += std::max(1, c.weight);
        if (total_weight <= 0)
            return 0;

        const uint64_t mix = pos.zobKey ^ (uint64_t(pos.fullmoveNumber) * 0x9e3779b97f4a7c15ULL);
        int pick = int(mix % uint64_t(total_weight));

        for (const auto& c : choices) {
            const int w = std::max(1, c.weight);
            if (pick < w)
                return find_legal_by_uci(pos, c.uci);
            pick -= w;
        }
        return 0;
    }

    Move pick_weighted(const Position& pos) const {
        const uint64_t mix = pos.zobKey ^ (uint64_t(pos.fullmoveNumber) * 0x9e3779b97f4a7c15ULL);
        auto extChoices = external_moves(pos);
        if (!extChoices.empty()) {
            Move m = choose_weighted(extChoices, mix);
            if (m)
                return m;
        }
        return pick_weighted_internal(pos);
    }

    Move pick_mainline_internal(const Position& pos) const {
        auto it = table_.find(pos.zobKey);
        if (it == table_.end() || it->second.empty())
            return 0;

        const auto& choices = it->second;
        int best_w = -1;
        Move best_m = 0;
        for (const auto& c : choices) {
            Move m = find_legal_by_uci(pos, c.uci);
            if (!m)
                continue;
            const int w = std::max(1, c.weight);
            if (w > best_w) {
                best_w = w;
                best_m = m;
            }
        }
        return best_m;
    }

    Move pick_mainline(const Position& pos) const {
        auto extChoices = external_moves(pos);
        if (!extChoices.empty()) {
            int bestW = -1;
            Move bestM = 0;
            for (const auto& c : extChoices) {
                if (c.weight > bestW) {
                    bestW = c.weight;
                    bestM = c.move;
                }
            }
            if (bestM)
                return bestM;
        }
        return pick_mainline_internal(pos);
    }

    std::vector<Move> build_pv(const Position& pos, Move first, int max_ply) const {
        std::vector<Move> pv;
        pv.reserve(64);
        if (!first)
            return pv;

        Position p = pos;
        const int cur = current_ply(pos);
        const int maxLen = std::max(1, std::min(64, max_ply - cur));
        pv.push_back(first);
        (void)p.do_move(first);

        while (current_ply(p) < max_ply && (int)pv.size() < maxLen) {
            Move next = pick_mainline(p);
            if (!next)
                break;
            pv.push_back(next);
            (void)p.do_move(next);
        }
        return pv;
    }

    void add_choice(const Position& pos, const std::string& uci, int weight) {
        if (weight <= 0)
            return;

        auto& lst = table_[pos.zobKey];
        for (auto& c : lst) {
            if (c.uci == uci) {
                c.weight += weight;
                return;
            }
        }
        lst.push_back(BookChoice{uci, weight});
    }

    void add_line(const std::vector<std::pair<std::string, int>>& plies) {
        Position p;
        p.set_startpos();

        for (const auto& entry : plies) {
            const std::string& uci = entry.first;
            const int weight = entry.second;

            add_choice(p, uci, weight);
            Move m = find_legal_by_uci(p, uci);
            if (!m)
                return;
            (void)p.do_move(m);
        }
    }

    void build() {
        Position p;
        p.set_startpos();
        if (Move e4 = find_legal_by_uci(p, "e2e4")) {
            (void)p.do_move(e4);
            key_after_e4_ = p.zobKey;
        }

        // A compact practical repertoire focused on common e4/d4 starts.
        add_line({{"e2e4", 45}, {"e7e5", 30}, {"g1f3", 28}, {"b8c6", 24}, {"f1b5", 18}, {"a7a6", 12},
                  {"b5a4", 10}, {"g8f6", 10}});
        add_line({{"e2e4", 45}, {"e7e5", 30}, {"g1f3", 25}, {"b8c6", 20}, {"f1c4", 16}, {"f8c5", 12}});
        add_line({{"e2e4", 45}, {"e7e5", 30}, {"g1f3", 20}, {"b8c6", 18}, {"d2d4", 14}, {"e5d4", 12},
                  {"f3d4", 10}});
        add_line({{"e2e4", 45}, {"c7c5", 26}, {"g1f3", 22}, {"d7d6", 18}, {"d2d4", 16}, {"c5d4", 14},
                  {"f3d4", 12}, {"g8f6", 10}});
        add_line({{"e2e4", 45}, {"c7c5", 20}, {"c2c3", 11}, {"d7d5", 10}, {"e4d5", 9}, {"d8d5", 8}});
        add_line({{"e2e4", 45}, {"e7e6", 16}, {"d2d4", 18}, {"d7d5", 16}, {"b1c3", 10}});
        add_line({{"e2e4", 45}, {"c7c6", 12}, {"d2d4", 18}, {"d7d5", 14}, {"b1c3", 10}});

        add_line({{"d2d4", 35}, {"d7d5", 28}, {"c2c4", 26}, {"e7e6", 16}, {"b1c3", 14}, {"g8f6", 12}});
        add_line({{"d2d4", 35}, {"g8f6", 24}, {"c2c4", 24}, {"e7e6", 16}, {"g1f3", 14}});
        add_line({{"d2d4", 35}, {"g8f6", 20}, {"c2c4", 20}, {"g7g6", 12}, {"b1c3", 12}, {"f8g7", 10}});
        add_line({{"d2d4", 35}, {"d7d5", 20}, {"g1f3", 16}, {"g8f6", 14}, {"e2e3", 10}});

        add_line({{"c2c4", 16}, {"e7e5", 12}, {"b1c3", 10}, {"g8f6", 10}});
        add_line({{"g1f3", 12}, {"d7d5", 10}, {"d2d4", 10}, {"g8f6", 10}});

        // Balance first-move alternatives.
        add_line({{"e2e4", 20}, {"c7c5", 12}, {"b1c3", 10}});
        add_line({{"d2d4", 14}, {"g8f6", 10}, {"g1f3", 10}});
    }
};

inline OpeningBook& instance() {
    static OpeningBook g_book;
    return g_book;
}

inline ProbeResult probe(const Position& pos, int max_ply) {
    return instance().probe(pos, max_ply);
}

inline bool set_book_file(const std::string& path) {
    return instance().set_external_book_file(path);
}

inline std::string current_book_file() {
    return instance().external_book_file();
}

inline bool external_book_loaded() {
    return instance().external_book_loaded();
}

} // namespace book
