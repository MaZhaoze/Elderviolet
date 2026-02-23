    inline bool is_legal_move_here(const Position& pos, Move m, int /*plyCtx*/) {
        if (!m)
            return false;
        if (collect_stats())
            ss.legCalls++;
        Position p = pos;
        std::vector<Move> legal;
        legal.reserve(256);
        movegen::generate_legal(p, legal);
        const bool ok = (std::find(legal.begin(), legal.end(), m) != legal.end());
        if (collect_stats()) {
            if (!ok)
                ss.legFail++;
            else {
                const bool cap = is_capture(pos, m);
                if (cap)
                    ss.legCapture++;
                else
                    ss.legQuiet++;
                if (flags_of(m) & MF_EP)
                    ss.legEp++;
                if (type_of(pos.board[from_sq(m)]) == KING)
                    ss.legKing++;
            }
        }
        return ok;
    }
