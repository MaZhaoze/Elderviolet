    int negamax(Position& pos, int depth, int alpha, int beta, int ply, int /*prevFrom*/, int /*prevTo*/, int lastTo,
                bool lastWasCap, PVLine& outPV) {
        outPV.len = 0;
        if (stop_or_time_up(false))
            return eval::evaluate(pos);

        if (ply >= MAX_PLY - 2)
            return eval::evaluate(pos);

        add_node();

        if (depth <= 0)
            return qsearch(pos, alpha, beta, ply, lastTo, lastWasCap);

        std::vector<Move>& mv = plyMoves[ply];
        mv.clear();
        movegen::generate_legal(pos, mv);
        if (mv.empty()) {
            if (attacks::in_check(pos, pos.side))
                return -MATE + ply;
            return 0;
        }

        Move ttMove = 0;
        TTEntry te{};
        if (stt->probe_copy(pos.zobKey, te)) {
            ss.ttProbe++;
            ss.ttHit++;
            ttMove = te.best;
            if (te.depth >= depth) {
                if (te.flag == TT_EXACT) {
                    ss.ttCut++;
                    return te.score;
                }
                if (te.flag == TT_ALPHA && te.score <= alpha) {
                    ss.ttCut++;
                    return te.score;
                }
                if (te.flag == TT_BETA && te.score >= beta) {
                    ss.ttCut++;
                    return te.score;
                }
            }
            if (ttMove)
                ss.ttMoveAvail++;
        }

        std::vector<int>& sc = plyScores[ply];
        std::vector<int>& ord = plyOrder[ply];
        sc.resize(mv.size());
        ord.resize(mv.size());
        for (int i = 0; i < (int)mv.size(); i++) {
            sc[i] = move_score(pos, mv[i], ttMove, ply, -1, -1);
            ord[i] = i;
        }
        std::sort(ord.begin(), ord.end(), [&](int a, int b) { return sc[a] > sc[b]; });

        int bestScore = -INF;
        Move bestMove = 0;
        PVLine bestChild{};
        int legalSearched = 0;

        for (int oi = 0; oi < (int)ord.size(); oi++) {
            const Move m = mv[ord[oi]];
            const bool cap = is_capture(pos, m);
            Undo u = do_move_counted(pos, m);
            legalSearched++;

            PVLine child{};
            int score = -INF;
            if (legalSearched == 1) {
                score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, from_sq(m), to_sq(m), to_sq(m), cap, child);
            } else {
                score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1, from_sq(m), to_sq(m), to_sq(m), cap, child);
                if (score > alpha && score < beta) {
                    PVLine child2{};
                    score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, from_sq(m), to_sq(m), to_sq(m), cap, child2);
                    child = child2;
                }
            }
            pos.undo_move(m, u);

            if (score > bestScore) {
                bestScore = score;
                bestMove = m;
                bestChild = child;
            }
            if (score > alpha)
                alpha = score;
            if (alpha >= beta) {
                ps.betaCutoff++;
                if (!cap) {
                    const int p = std::min(ply, 127);
                    if (killer[0][p] != m) {
                        killer[1][p] = killer[0][p];
                        killer[0][p] = m;
                    }
                    const int ci = color_index(pos.side);
                    update_stat(history[ci][from_sq(m)][to_sq(m)], 1200 + depth * depth * 20);
                }
                break;
            }
        }

        outPV.m[0] = bestMove;
        outPV.len = (bestMove ? 1 : 0);
        for (int i = 0; i < bestChild.len && outPV.len < 128; i++)
            outPV.m[outPV.len++] = bestChild.m[i];

        if (bestMove) {
            const uint8_t fl = (bestScore >= beta) ? TT_BETA : ((bestScore <= alpha) ? TT_ALPHA : TT_EXACT);
            stt->store(pos.zobKey, bestMove, (int16_t)bestScore, (int16_t)depth, fl);
        }

        return bestScore;
    }
