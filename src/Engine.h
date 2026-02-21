// Engine: owns position state and coordinates search / UCI-facing behavior.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>

#include "types.h"
#include "Position.h"
#include "MoveGeneration.h"
#include "Search.h"

class Engine {
  public:
    Engine() { pos.set_startpos(); }

    ~Engine() { stop(); }

    void new_game() {
        stop();
        pos.set_startpos();
        search::clear_tt();
        last_ponder_move_.store(0, std::memory_order_relaxed);
    }

    // Options exposed via UCI.
    void set_hash(int mb) { search::set_hash_mb(mb); }

    void set_threads(int n) {
        threads_ = std::max(1, n);
        search::set_threads(threads_);
    }
    // Currently cached only (search output is single-PV).
    void set_multipv(int n) { multipv_ = std::max(1, n); }
    // Cached for UCI go ponder.
    void set_ponder(bool b) { ponder_ = b; }
    void set_move_overhead(int ms) { move_overhead_ms_ = std::max(0, ms); }
    int move_overhead_ms() const { return move_overhead_ms_; }
    void set_syzygy_path(const std::string& s) { syzygy_path_ = s; }
    void set_skill_level(int lv) { skill_level_ = std::max(0, std::min(20, lv)); }
    int skill_level() const { return skill_level_; }
    void set_search_stats(bool on) { search::set_collect_stats(on); }

    // Position management.
    void set_startpos() { pos.set_startpos(); }
    void set_fen(const std::string& fen) { pos.set_fen(fen); }
    Color side_to_move() const { return pos.side; }

    // Stop any ongoing search and join background thread.
    void stop() {
        search::stop();

        if (searching_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> g(bg_mtx_);
            if (bg_thread_.joinable())
                bg_thread_.join();
            searching_.store(false, std::memory_order_release);
            pondering_.store(false, std::memory_order_release);
        }
    }

    // UCI: ponderhit. Stop background ponder search and keep its last result.
    void ponderhit() {
        if (!pondering_.load(std::memory_order_acquire))
            return;
        stop();
    }

    // Apply a UCI move string if it matches a legal move.
    void push_uci_move(const std::string& uciMove) {
        std::vector<Move> moves;
        moves.reserve(256);
        movegen::generate_legal(pos, moves);

        for (Move m : moves) {
            if (move_to_uci(m) == uciMove) {
                Undo u = pos.do_move(m);
                (void)u;
                return;
            }
        }
    }

    // Last ponder move from the most recent search.
    int get_last_ponder_move() const { return last_ponder_move_.load(std::memory_order_relaxed); }

    // Start a search with UCI-style limits.
    // Precedence: ponder > infinite > movetime > clock > depth.
    int go(int depth, int movetime, bool infinite, int wtime, int btime, int winc, int binc, int movestogo,
           bool ponder) {
        // Stop any existing background search to avoid re-entrancy.
        stop();

        search::Limits lim{};
        lim.depth = 0;
        lim.movetime_ms = 0;
        lim.infinite = false;

        const bool depth_given = (depth > 0);
        const bool movetime_given = (movetime > 0);

        const bool hasClock = (wtime > 0) || (btime > 0) || (winc > 0) || (binc > 0) || (movestogo > 0);

        // Ponder: run an infinite search in the background and return no bestmove yet.
        if (ponder) {
            lim.infinite = true;
            lim.movetime_ms = 0;
            lim.depth = depth_given ? depth : 0;

            start_background_search(lim);
            pondering_.store(true, std::memory_order_release);
            return 0; // UCI: do not output bestmove on ponder start.
        }

        // 1) infinite
        if (infinite) {
            lim.infinite = true;
            lim.movetime_ms = 0;
            lim.depth = depth_given ? depth : 0;

            search::Result r = search::think(pos, lim);
            last_ponder_move_.store((int)r.ponderMove, std::memory_order_relaxed);
            return (int)r.bestMove;
        }

        // 2) movetime takes absolute precedence (after ponder/infinite).
        if (movetime_given) {
            lim.infinite = false;
            lim.movetime_ms = std::max(1, movetime);
            lim.depth = depth_given ? depth : 0;

            search::Result r = search::think(pos, lim);
            last_ponder_move_.store((int)r.ponderMove, std::memory_order_relaxed);
            return (int)r.bestMove;
        }

        // 3) Clock mode: use wtime/btime and increments if provided.
        if (hasClock) {
            int myTime = (pos.side == WHITE ? wtime : btime);
            int myInc = (pos.side == WHITE ? winc : binc);

            if (myTime < 0)
                myTime = 0;
            if (myInc < 0)
                myInc = 0;

            int t_ms = compute_think_ms(myTime, myInc, movestogo, move_overhead_ms_);
            if (t_ms < 1)
                t_ms = 1;

            lim.movetime_ms = t_ms;
            lim.infinite = false;
        } else {
            // 4) Depth-only search (no clock, no movetime).
            lim.movetime_ms = 0;
            lim.infinite = false;
        }

        // Depth strategy.
        lim.depth = depth_given ? depth : 0;

        // Skill Level: cap depth and time only in clock mode without a forced depth.
        if (!depth_given && hasClock && skill_level_ < 20) {
            int capDepth = 4 + skill_level_ / 2; // 0..19 -> 4..13
            capDepth = std::max(1, std::min(64, capDepth));
            lim.depth = capDepth;

            int factor = 40 + (skill_level_ * 50) / 19; // 40..90
            lim.movetime_ms = std::max(1, (lim.movetime_ms * factor) / 100);
        }

        search::Result r = search::think(pos, lim);
        last_ponder_move_.store((int)r.ponderMove, std::memory_order_relaxed);
        return (int)r.bestMove;
    }

    // Convert an encoded move to UCI coordinate notation.
    std::string move_to_uci(int m) const { return move_to_uci((Move)m); }

    std::string move_to_uci(Move m) const {
        int from = from_sq(m);
        int to = to_sq(m);

        std::string s;
        s += char('a' + file_of(from));
        s += char('1' + rank_of(from));
        s += char('a' + file_of(to));
        s += char('1' + rank_of(to));

        int promo = promo_of(m);
        if (promo) {
            char pc = 'q';
            if (promo == 1)
                pc = 'n';
            else if (promo == 2)
                pc = 'b';
            else if (promo == 3)
                pc = 'r';
            else if (promo == 4)
                pc = 'q';
            s += pc;
        }
        return s;
    }

  private:
    // Time allocation for clock mode only (ms).
    static inline int compute_think_ms(int mytime_ms, int myinc_ms, int movestogo, int move_overhead_ms) {
        if (mytime_ms <= 0)
            return 1;

        int tleft = mytime_ms - std::max(0, move_overhead_ms);
        if (tleft < 1)
            tleft = 1;

        if (tleft <= 200) {
            return std::max(1, tleft / 4);
        }

        int inc_part = (myinc_ms * 85) / 100;

        int t = 0;
        if (movestogo > 0) {
            int mtg = std::max(1, movestogo);
            int base = tleft / mtg;
            t = base + inc_part;
            t = std::min(t, (tleft * 60) / 100);
        } else {
            int base = tleft / 30;
            t = base + inc_part;
            t = std::min(t, tleft / 2);
        }

        t = std::max(t, 5);
        if (t > 2)
            t -= 2;
        return t;
    }

    void start_background_search(const search::Limits& lim) {
        // Work on a copy of the current position to avoid mutating engine state.
        Position pcopy = pos;

        searching_.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> g(bg_mtx_);
        bg_thread_ = std::thread([this, pcopy, lim]() mutable {
            // Background search does not emit info to avoid interleaving.
            search::Result r = search::think(pcopy, lim);
            last_best_move_.store((int)r.bestMove, std::memory_order_relaxed);
            last_ponder_move_.store((int)r.ponderMove, std::memory_order_relaxed);
            searching_.store(false, std::memory_order_release);
        });
    }

  private:
    Position pos;

    // options cache
    int threads_ = 1;
    int multipv_ = 1;
    bool ponder_ = false;
    int move_overhead_ms_ = 10;
    std::string syzygy_path_;
    int skill_level_ = 20;

    // ponder/search state
    std::atomic<bool> searching_{false};
    std::atomic<bool> pondering_{false};
    std::atomic<int> last_best_move_{0};
    std::atomic<int> last_ponder_move_{0};

    std::thread bg_thread_;
    std::mutex bg_mtx_;
};
