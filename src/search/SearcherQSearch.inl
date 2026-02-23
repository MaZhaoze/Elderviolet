    int qsearch(Position& pos, int alpha, int beta, int ply, int /*lastTo*/, bool /*lastWasCap*/) {
        add_node();
        if (ply >= MAX_PLY - 2)
            return eval::evaluate(pos);

        int stand = eval::evaluate(pos);
        if (stand >= beta)
            return stand;
        if (stand > alpha)
            alpha = stand;

        std::vector<Move>& mv = plyMoves[ply];
        mv.clear();
        movegen::generate_legal(pos, mv);

        static const int V[7] = {0, 100, 320, 330, 500, 900, 0};

        for (Move m : mv) {
            const bool cap = is_capture(pos, m);
            const bool promo = (promo_of(m) != 0);
            if (!(cap || promo))
                continue;

            int gain = 0;
            if (cap) {
                Piece victim = pos.board[to_sq(m)];
                if (flags_of(m) & MF_EP)
                    victim = make_piece(flip_color(pos.side), PAWN);
                gain += (victim == NO_PIECE) ? 0 : V[type_of(victim)];
            }
            if (promo)
                gain += 800;

            if (stand + gain + 100 <= alpha)
                continue;

            Undo u = do_move_counted(pos, m, true);
            int score = -qsearch(pos, -beta, -alpha, ply + 1, to_sq(m), true);
            pos.undo_move(m, u);
            if (score >= beta)
                return score;
            if (score > alpha)
                alpha = score;
        }
        return alpha;
    }
