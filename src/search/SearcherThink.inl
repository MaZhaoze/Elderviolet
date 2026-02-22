    // Iterative deepening with aspiration windows and root move ordering.
    Result think(Position& pos, const Limits& lim, bool emitInfo) {
        isMainSearchThread = emitInfo;
        keyPly = 0;
        keyStack[keyPly++] = pos.zobKey;
        for (int i = 0; i < MAX_PLY; i++) {
            staticEvalStack[i] = -INF;
            pinnedMaskValid[i] = false;
            inCheckCacheValid[i] = false;
        }

        Result res{};
        nodes = 0;
        nodes_batch = 0;
        time_check_tick = 0;
        selDepth = 0;
        ps.clear();
        ss.clear();

        const int maxDepth = (lim.depth > 0 ? std::min(lim.depth, MAX_PLY - 1) : (MAX_PLY - 1));
        const int64_t startT = now_ms();
        int lastFlushMs = 0;
        int lastInfoMs = -1000000;

        std::vector<Move> rootMoves;
        rootMoves.reserve(256);
        movegen::generate_legal(pos, rootMoves);

        if (rootMoves.empty()) {
            flush_nodes_batch();
            res.bestMove = 0;
            res.ponderMove = 0;
            res.score = 0;
            res.nodes = nodes;
            return res;
        }

        Move bestMove = rootMoves[0];
        int bestScore = -INF;

        constexpr int ASP_START = 35;
        constexpr int PV_MAX = 128;
        // Combine global nodes and local batch for consistent info output.
        auto now_time_nodes_nps = [&]() {
            int t = (int)(now_ms() - startT);
            if (t < 1)
                t = 1;

            uint64_t nodesAll = g_nodes_total.load(std::memory_order_relaxed) + nodes_batch;

            uint64_t npsAll = (nodesAll * 1000ULL) / (uint64_t)t;

            return std::tuple<int, uint64_t, uint64_t>(t, npsAll, nodesAll);
        };

        PVLine rootPV;
        PVLine rootPVLegal;

        auto classify_root_source = [&](Move m, Move ttM) {
            if (m && ttM && m == ttM)
                return 0; // tt
            const bool cap = is_capture(pos, m) || (flags_of(m) & MF_EP) || promo_of(m);
            if (cap)
                return 1; // capture
            if (m == killer[0][0] || m == killer[1][0])
                return 2; // killer
            // Root has no natural predecessor, so countermove at root is generally unavailable.
            return 4; // quiet
        };

        // Root search with PVS and late-move reductions (root-specific heuristics).
        auto root_search = [&](int d, int alpha, int beta, Move& outBestMove, int& outBestScore, PVLine& outPV,
                               bool& outOk) {
            outOk = true;

            int curAlpha = alpha;
            int curBeta = beta;

            Move iterBestMove = 0;
            int iterBestScore = -INF;
            PVLine iterPV;
            iterPV.len = 0;
            int iterBestIndex = -1;
            bool firstMoveCut = false;

            int rootLegalsSearched = 0;
            const bool splitActive = (!isMainSearchThread && rootSplitStride > 1 &&
                                      (int)rootMoves.size() >= rootSplitStride);

            for (int i = 0; i < (int)rootMoves.size(); i++) {
                if (splitActive && ((i % rootSplitStride) != rootSplitOffset))
                    continue;
                if (stop_or_time_up(true)) [[unlikely]] {
                    outOk = false;
                    break;
                }

                Move m = rootMoves[i];
                const uint16_t flags = flags_of(m);
                const int to = to_sq(m);

                const bool isCap = ((flags & MF_EP) != 0) || (pos.board[to] != NO_PIECE);
                const bool isPromo = (promo_of(m) != 0);

                Undo u = do_move_counted(pos, m);

                rootLegalsSearched++;

                bool givesCheck = false;
                if (d >= 6 && i >= 4) {
                    givesCheck = attacks::in_check(pos, pos.side);
                }

                const int nextLastTo = to;
                const bool nextLastWasCap = isCap;

                int score = -INF;

                int r = 0;
                if (!isCap && !isPromo && !givesCheck && d >= 6 && i >= 4) {
                    r = 1;
                    if (d >= 10 && i >= 10)
                        r = 2;
                    r = std::min(r, d - 2);
                }

                PVLine childPV;

                const int curFrom = from_sq(m);
                const int curTo = to_sq(m);

                if (rootLegalsSearched == 1) {
                    score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV);
                } else {
                    if (collect_stats())
                        ss.rootNonFirstTried++;
                    int rd = (d - 1) - r;
                    if (rd < 0)
                        rd = 0;

                    score = -negamax(pos, rd, -curAlpha - 1, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                     childPV);

                    if (score > curAlpha && score < curBeta) {
                        if (collect_stats()) {
                            ss.rootPvsReSearch++;
                            if (r > 0)
                                ss.rootLmrReSearch++;
                        }
                        PVLine childPV2;
                        score = -negamax(pos, d - 1, -curBeta, -curAlpha, 1, curFrom, curTo, nextLastTo, nextLastWasCap,
                                         childPV2);
                        childPV = childPV2;
                    }
                }

                pos.undo_move(m, u);

                if (stop_or_time_up(true)) [[unlikely]] {
                    outOk = false;
                    break;
                }

                if (score > iterBestScore) {
                    iterBestScore = score;
                    iterBestMove = m;
                    iterBestIndex = i;

                    iterPV.m[0] = m;
                    iterPV.len = std::min(127, childPV.len + 1);
                    for (int k = 0; k < childPV.len && k + 1 < 128; k++) {
                        iterPV.m[k + 1] = childPV.m[k];
                    }
                }

                if (score > curAlpha)
                    curAlpha = score;
                if (curAlpha >= curBeta) {
                    if (i == 0)
                        firstMoveCut = true;
                    break;
                }
            }

            if (rootLegalsSearched == 0)
                outOk = false;

            if (collect_stats() && rootLegalsSearched > 0) {
                ss.rootIters++;
                if (iterBestIndex == 0 || firstMoveCut)
                    ss.rootFirstBestOrCut++;
            }

            outBestMove = iterBestMove;
            outBestScore = iterBestScore;
            outPV = iterPV;
        };

        Move prevIterBestMove = 0;
        int prevIterScore = 0;
        bool prevHadNull = false;
        bool prevHadRfp = false;
        bool prevHadRazor = false;
        Move softPrevBestMove = 0;
        int softStableIters = 0;

        for (int d = 1; d <= maxDepth; d++) {
            if (stop_or_time_up(true)) [[unlikely]]
                break;

            if (bestMove) {
                auto it = std::find(rootMoves.begin(), rootMoves.end(), bestMove);
                if (it != rootMoves.end())
                    std::swap(rootMoves[0], *it);
            }

            std::vector<int> rootScores(rootMoves.size());
            for (int i = 0; i < (int)rootMoves.size(); i++)
                rootScores[i] = move_score(pos, rootMoves[i], bestMove, 0, -1, -1);

            const int K = std::min<int>(g_params.rootOrderK, (int)rootMoves.size());
            for (int i = 0; i < K; i++) {
                int bi = i, bs = rootScores[i];
                for (int j = i + 1; j < (int)rootMoves.size(); j++) {
                    if (rootScores[j] > bs) {
                        bs = rootScores[j];
                        bi = j;
                    }
                }
                if (bi != i) {
                    std::swap(rootMoves[i], rootMoves[bi]);
                    std::swap(rootScores[i], rootScores[bi]);
                }
            }

            const bool useAsp = (d > 5 && bestScore > -INF / 2 && bestScore < INF / 2);
            int asp = ASP_START;
            const int scoreSwing = std::abs(bestScore - prevIterScore);
            asp += std::min(64, scoreSwing / 4);
            if (!isMainSearchThread) {
                // Stockfish-inspired per-thread aspiration diversification:
                // wider and slightly jittered windows reduce same-line re-search overlap.
                asp += 24 + ((threadIndex & 7) << 2);
            }
            int alpha = useAsp ? (bestScore - asp) : -INF;
            int beta = useAsp ? (bestScore + asp) : INF;

            Move localBestMove = bestMove;
            int localBestScore = bestScore;
            PVLine localPV;
            localPV.len = 0;
            bool ok = false;
            bool iterAspFailed = false;
            Move rootTTMove = 0;
            {
                TTEntry rte{};
                if (stt->probe_copy(pos.zobKey, rte) && rte.best && is_legal_move_here(pos, rte.best, 0))
                    rootTTMove = rte.best;
            }

            const uint64_t pRazor0 = ps.razorPrune;
            const uint64_t pRfp0 = ps.rfpPrune;
            const uint64_t pNull0 = ss.nullTried;

            root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
            if (!ok)
                break;

            if (useAsp && (localBestScore <= alpha || localBestScore >= beta)) {
                if (collect_stats())
                    ss.aspFail++;
                iterAspFailed = true;
                if (isMainSearchThread) {
                    alpha = -INF;
                    beta = INF;
                    root_search(d, alpha, beta, localBestMove, localBestScore, localPV, ok);
                    if (!ok)
                        break;
                }
            }

            // If current root PV is too short, rebuild it with a single full-window
            // confirmation on the current best move.
            if (isMainSearchThread && ok && localBestMove && d >= 8 && localPV.len < 4 && !stop_or_time_up(true)) {
                const int bf = from_sq(localBestMove);
                const int bt = to_sq(localBestMove);
                const bool bcap = is_capture(pos, localBestMove) || (flags_of(localBestMove) & MF_EP);
                Undo u = do_move_counted(pos, localBestMove);
                PVLine fullChild;
                int fullScore = -negamax(pos, d - 1, -INF, INF, 1, bf, bt, bt, bcap, fullChild);
                pos.undo_move(localBestMove, u);
                if (!stop_or_time_up(true)) {
                    localBestScore = fullScore;
                    localPV.m[0] = localBestMove;
                    localPV.len = std::min(127, fullChild.len + 1);
                    for (int k = 0; k < fullChild.len && k + 1 < 128; k++)
                        localPV.m[k + 1] = fullChild.m[k];
                }
            }

            if (collect_stats()) {
                ss.rootBestSrc[classify_root_source(localBestMove, rootTTMove)]++;
                const bool hadRazor = (ps.razorPrune > pRazor0);
                const bool hadRfp = (ps.rfpPrune > pRfp0);
                const bool hadNull = (ss.nullTried > pNull0);
                if (prevIterBestMove && localBestMove && localBestMove != prevIterBestMove &&
                    std::abs(localBestScore - prevIterScore) >= 120) {
                    if (prevHadNull)
                        ss.proxyReversalAfterNull++;
                    if (prevHadRfp)
                        ss.proxyReversalAfterRfp++;
                    if (prevHadRazor)
                        ss.proxyReversalAfterRazor++;
                }
                prevHadNull = hadNull;
                prevHadRfp = hadRfp;
                prevHadRazor = hadRazor;
                prevIterBestMove = localBestMove;
                prevIterScore = localBestScore;
            }

            bestMove = localBestMove;
            bestScore = localBestScore;
            rootPV = localPV;

            if (bestMove && bestMove == softPrevBestMove)
                softStableIters++;
            else {
                softPrevBestMove = bestMove;
                softStableIters = 0;
            }

            if (emitInfo) {
                auto [t, nps, nodesAll] = now_time_nodes_nps();
                rootPVLegal = sanitize_pv_from_root(pos, rootPV, PV_MAX);
                int hashfull = stt->hashfull_permille();
                int sd = std::max(1, selDepth);

                std::cout << "info depth " << d << " seldepth " << sd << " multipv 1 ";
                print_score_uci(bestScore);
                std::cout << " nodes " << nodesAll << " nps " << nps << " hashfull " << hashfull << " tbhits 0"
                          << " time " << t << " pv ";

                int outN = std::min(PV_MAX, rootPVLegal.len);
                for (int i = 0; i < outN; i++) {
                    Move pm = rootPVLegal.m[i];
                    if (!pm)
                        break;
                    std::cout << move_to_uci(pm) << " ";
                }
                std::cout << "\n";

                int curMs = t;
                if (curMs - lastFlushMs >= 50) {
                    std::cout.flush();
                    lastFlushMs = curMs;
                }
                lastInfoMs = t;
            }

            // Soft time budget: stop only when PV is reasonably stable.
            if (soft_time_up()) [[unlikely]] {
                const bool deepEnough = (d >= 8);
                const bool stableEnough = (softStableIters >= 1);
                if (deepEnough && stableEnough && !iterAspFailed)
                    break;
            }
        }

        flush_nodes_batch();

        res.bestMove = bestMove;
        res.score = bestScore;
        res.nodes = nodes;

        rootPVLegal = sanitize_pv_from_root(pos, rootPV, PV_MAX);
        res.ponderMove = (rootPVLegal.len >= 2 ? rootPVLegal.m[1] : 0);

        if (emitInfo) {
            std::cout << "info string prune razor=" << ps.razorPrune << " rfp=" << ps.rfpPrune
                      << " pcut=" << ps.probCutPrune
                      << " qfut=" << ps.quietFutility << " qlim=" << ps.quietLimit
                      << " csee=" << ps.capSeePrune << " iir=" << ps.iirApplied << " lmr=" << ps.lmrApplied
                      << " bcut=" << ps.betaCutoff << "\n";
            if (collect_stats()) {
                uint64_t rootDen = ss.rootIters ? ss.rootIters : 1;
                uint64_t ttDen = ss.ttProbe ? ss.ttProbe : 1;
                uint64_t ttMoveDen = ss.ttMoveAvail ? ss.ttMoveAvail : 1;
                uint64_t lmrDen = ss.lmrTried ? ss.lmrTried : 1;
                uint64_t rootReDen = ss.rootNonFirstTried ? ss.rootNonFirstTried : 1;

                auto pct = [](uint64_t num, uint64_t den) { return (1000ULL * num) / (den ? den : 1); };
                uint64_t avgPv = (ss.nodeByType[0] ? (1000ULL * ss.legalByType[0] / ss.nodeByType[0]) : 0);
                uint64_t avgCut = (ss.nodeByType[1] ? (1000ULL * ss.legalByType[1] / ss.nodeByType[1]) : 0);
                uint64_t avgAll = (ss.nodeByType[2] ? (1000ULL * ss.legalByType[2] / ss.nodeByType[2]) : 0);

                std::cout << "info string stats_root fh1=" << pct(ss.rootFirstBestOrCut, rootDen)
                          << " re=" << pct(ss.rootPvsReSearch, rootReDen)
                          << " src_tt=" << pct(ss.rootBestSrc[0], rootDen) << " src_cap=" << pct(ss.rootBestSrc[1], rootDen)
                          << " src_k=" << pct(ss.rootBestSrc[2], rootDen) << " src_c=" << pct(ss.rootBestSrc[3], rootDen)
                          << " src_q=" << pct(ss.rootBestSrc[4], rootDen) << " asp=" << ss.aspFail << "\n";

                std::cout << "info string stats_node pv=" << ss.nodeByType[0] << " cut=" << ss.nodeByType[1]
                          << " all=" << ss.nodeByType[2] << " avgm_pv=" << avgPv << " avgm_cut=" << avgCut
                          << " avgm_all=" << avgAll << " tt_hit=" << pct(ss.ttHit, ttDen)
                          << " tt_cut=" << pct(ss.ttCut, ttDen) << " ttm_first=" << pct(ss.ttMoveFirst, ttMoveDen)
                          << "\n";

                std::cout << "info string stats_lmr red=" << ss.lmrTried << " re=" << pct(ss.lmrResearched, lmrDen)
                          << " rk=" << ss.lmrReducedByBucket[0] << " rc=" << ss.lmrReducedByBucket[1]
                          << " rh=" << ss.lmrReducedByBucket[2] << " rl=" << ss.lmrReducedByBucket[3]
                          << " rek=" << ss.lmrResearchedByBucket[0] << " rec=" << ss.lmrResearchedByBucket[1]
                          << " reh=" << ss.lmrResearchedByBucket[2] << " rel=" << ss.lmrResearchedByBucket[3] << "\n";

                std::cout << "info string stats_prune null_t=" << ss.nullTried << " null_fh=" << ss.nullCut
                          << " null_vf=" << ss.nullVerifyFail << " raz=" << ps.razorPrune << " rfp=" << ps.rfpPrune
                          << " rev_null=" << ss.proxyReversalAfterNull << " rev_rfp=" << ss.proxyReversalAfterRfp
                          << " rev_raz=" << ss.proxyReversalAfterRazor << " tchk=" << ss.timeChecks
                          << " leg=" << ss.legCalls << " legf=" << ss.legFail << " seem=" << ss.seeCallsMain
                          << " seeq=" << ss.seeCallsQ << " seefs=" << ss.seeFastSafe << " mk=" << ss.makeCalls
                          << " mkm=" << ss.makeMain << " mkq=" << ss.makeQ << " pinc=" << ss.pinCalc << "\n";

                const uint64_t legDen = ss.legCalls ? ss.legCalls : 1;
                std::cout << "info string stats_leg failr=" << pct(ss.legFail, legDen)
                          << " q=" << pct(ss.legQuiet, legDen) << " c=" << pct(ss.legCapture, legDen)
                          << " chk=" << pct(ss.legCheck, legDen) << " ep=" << pct(ss.legEp, legDen)
                          << " king=" << pct(ss.legKing, legDen) << " sus=" << pct(ss.legSuspin, legDen)
                          << " fast=" << pct(ss.legFast, legDen) << " fast2=" << pct(ss.legFast2, legDen) << "\n";

                auto sum_bucket = [&](uint64_t a[3][SearchStats::BUCKET_N], int b) {
                    return a[0][b] + a[1][b] + a[2][b];
                };
                const uint64_t t0 = sum_bucket(ss.bucketTry, MB_TT), t1 = sum_bucket(ss.bucketTry, MB_CAP_GOOD),
                               t2 = sum_bucket(ss.bucketTry, MB_QUIET_SPECIAL), t3 = sum_bucket(ss.bucketTry, MB_QUIET),
                               t4 = sum_bucket(ss.bucketTry, MB_CAP_BAD);
                const uint64_t f0 = sum_bucket(ss.bucketFh, MB_TT), f1 = sum_bucket(ss.bucketFh, MB_CAP_GOOD),
                               f2 = sum_bucket(ss.bucketFh, MB_QUIET_SPECIAL), f3 = sum_bucket(ss.bucketFh, MB_QUIET),
                               f4 = sum_bucket(ss.bucketFh, MB_CAP_BAD);
                const uint64_t b0 = sum_bucket(ss.bucketBest, MB_TT), b1 = sum_bucket(ss.bucketBest, MB_CAP_GOOD),
                               b2 = sum_bucket(ss.bucketBest, MB_QUIET_SPECIAL), b3 = sum_bucket(ss.bucketBest, MB_QUIET),
                               b4 = sum_bucket(ss.bucketBest, MB_CAP_BAD);
                const uint64_t s0 = sum_bucket(ss.bucketSee, MB_TT), s1 = sum_bucket(ss.bucketSee, MB_CAP_GOOD),
                               s2 = sum_bucket(ss.bucketSee, MB_QUIET_SPECIAL), s3 = sum_bucket(ss.bucketSee, MB_QUIET),
                               s4 = sum_bucket(ss.bucketSee, MB_CAP_BAD);
                const uint64_t l0 = sum_bucket(ss.bucketLeg, MB_TT), l1 = sum_bucket(ss.bucketLeg, MB_CAP_GOOD),
                               l2 = sum_bucket(ss.bucketLeg, MB_QUIET_SPECIAL), l3 = sum_bucket(ss.bucketLeg, MB_QUIET),
                               l4 = sum_bucket(ss.bucketLeg, MB_CAP_BAD);
                const uint64_t m0 = sum_bucket(ss.bucketMk, MB_TT), m1 = sum_bucket(ss.bucketMk, MB_CAP_GOOD),
                               m2 = sum_bucket(ss.bucketMk, MB_QUIET_SPECIAL), m3 = sum_bucket(ss.bucketMk, MB_QUIET),
                               m4 = sum_bucket(ss.bucketMk, MB_CAP_BAD);
                auto phr = [&](uint64_t fh, uint64_t tr) { return pct(fh, tr ? tr : 1); };
                std::cout << "info string stats_bucket"
                          << " tt_t=" << t0 << " tt_fh=" << f0 << " tt_fhr=" << phr(f0, t0) << " tt_best=" << b0
                          << " tt_see=" << s0 << " tt_leg=" << l0 << " tt_mk=" << m0
                          << " cg_t=" << t1 << " cg_fh=" << f1 << " cg_fhr=" << phr(f1, t1) << " cg_best=" << b1
                          << " cg_see=" << s1 << " cg_leg=" << l1 << " cg_mk=" << m1
                          << " qs_t=" << t2 << " qs_fh=" << f2 << " qs_fhr=" << phr(f2, t2) << " qs_best=" << b2
                          << " qs_see=" << s2 << " qs_leg=" << l2 << " qs_mk=" << m2
                          << " q_t=" << t3 << " q_fh=" << f3 << " q_fhr=" << phr(f3, t3) << " q_best=" << b3
                          << " q_see=" << s3 << " q_leg=" << l3 << " q_mk=" << m3
                          << " cb_t=" << t4 << " cb_fh=" << f4 << " cb_fhr=" << phr(f4, t4) << " cb_best=" << b4
                          << " cb_see=" << s4 << " cb_leg=" << l4 << " cb_mk=" << m4 << "\n";

                auto phr_cut = [&](int b) {
                    const uint64_t tr = ss.bucketTry[1][b];
                    const uint64_t fh = ss.bucketFh[1][b];
                    return pct(fh, tr ? tr : 1);
                };
                std::cout << "info string stats_bucket_cut"
                          << " tt_t=" << ss.bucketTry[1][MB_TT] << " tt_fh=" << ss.bucketFh[1][MB_TT]
                          << " tt_fhr=" << phr_cut(MB_TT) << " tt_best=" << ss.bucketBest[1][MB_TT]
                          << " tt_see=" << ss.bucketSee[1][MB_TT] << " tt_leg=" << ss.bucketLeg[1][MB_TT]
                          << " tt_mk=" << ss.bucketMk[1][MB_TT]
                          << " cg_t=" << ss.bucketTry[1][MB_CAP_GOOD] << " cg_fh=" << ss.bucketFh[1][MB_CAP_GOOD]
                          << " cg_fhr=" << phr_cut(MB_CAP_GOOD) << " cg_best=" << ss.bucketBest[1][MB_CAP_GOOD]
                          << " cg_see=" << ss.bucketSee[1][MB_CAP_GOOD] << " cg_leg=" << ss.bucketLeg[1][MB_CAP_GOOD]
                          << " cg_mk=" << ss.bucketMk[1][MB_CAP_GOOD]
                          << " qs_t=" << ss.bucketTry[1][MB_QUIET_SPECIAL] << " qs_fh=" << ss.bucketFh[1][MB_QUIET_SPECIAL]
                          << " qs_fhr=" << phr_cut(MB_QUIET_SPECIAL) << " qs_best=" << ss.bucketBest[1][MB_QUIET_SPECIAL]
                          << " qs_see=" << ss.bucketSee[1][MB_QUIET_SPECIAL] << " qs_leg=" << ss.bucketLeg[1][MB_QUIET_SPECIAL]
                          << " qs_mk=" << ss.bucketMk[1][MB_QUIET_SPECIAL]
                          << " q_t=" << ss.bucketTry[1][MB_QUIET] << " q_fh=" << ss.bucketFh[1][MB_QUIET]
                          << " q_fhr=" << phr_cut(MB_QUIET) << " q_best=" << ss.bucketBest[1][MB_QUIET]
                          << " q_see=" << ss.bucketSee[1][MB_QUIET] << " q_leg=" << ss.bucketLeg[1][MB_QUIET]
                          << " q_mk=" << ss.bucketMk[1][MB_QUIET]
                          << " cb_t=" << ss.bucketTry[1][MB_CAP_BAD] << " cb_fh=" << ss.bucketFh[1][MB_CAP_BAD]
                          << " cb_fhr=" << phr_cut(MB_CAP_BAD) << " cb_best=" << ss.bucketBest[1][MB_CAP_BAD]
                          << " cb_see=" << ss.bucketSee[1][MB_CAP_BAD] << " cb_leg=" << ss.bucketLeg[1][MB_CAP_BAD]
                          << " cb_mk=" << ss.bucketMk[1][MB_CAP_BAD] << "\n";
            }
        }

        return res;
    }
