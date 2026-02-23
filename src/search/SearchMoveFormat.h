#pragma once

#include <iostream>
#include <string>

#include "../types.h"

namespace search {

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

inline void print_score_uci(int score) {
    if (score >= MATE - 1024) {
        int plies = MATE - score;
        int mateIn = (plies + 1) / 2;
        std::cout << "score mate " << mateIn;
        return;
    }
    if (score <= -MATE + 1024) {
        int plies = MATE + score;
        int mateIn = (plies + 1) / 2;
        std::cout << "score mate -" << mateIn;
        return;
    }
    std::cout << "score cp " << score;
}

} // namespace search
