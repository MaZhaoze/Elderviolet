    inline int move_score(const Position& pos, Move m, Move ttMove, int ply, int /*prevFrom*/, int /*prevTo*/) {
        if (m == ttMove)
            return 2000000000;

        const bool cap = is_capture(pos, m);
        const int to = to_sq(m);
        const int from = from_sq(m);

        if (cap) {
            const Piece victim = (flags_of(m) & MF_EP) ? make_piece(flip_color(pos.side), PAWN) : pos.board[to];
            const Piece attacker = pos.board[from];
            return 1000000 + mvv_lva(victim, attacker);
        }

        int sc = 0;
        const int p = std::min(127, std::max(0, ply));
        if (killer[0][p] == m)
            sc += 900000;
        else if (killer[1][p] == m)
            sc += 800000;

        const int ci = color_index(pos.side);
        sc += history[ci][from][to];
        return sc;
    }
