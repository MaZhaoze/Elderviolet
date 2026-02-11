#pragma once
#include <string>
#include <vector>
#include <algorithm>

#include "types.h"
#include "Position.h"
#include "MoveGeneration.h"
#include "Search.h"

class Engine {
public:
    Engine() { pos.set_startpos(); }

    void new_game() {
        pos.set_startpos();
        search::clear_tt();
    }

    // ===== options =====
    void set_hash(int mb) { search::set_hash_mb(mb); }

    void set_threads(int n) {
        threads_ = std::max(1, n);
        search::set_threads(threads_);
    }
    void set_multipv(int n) { multipv_ = std::max(1, n); }     // 先存着
    void set_ponder(bool b) { ponder_ = b; }                   // 先存着
    void set_move_overhead(int ms) { move_overhead_ms_ = std::max(0, ms); }
    int  move_overhead_ms() const { return move_overhead_ms_; }
    void set_syzygy_path(const std::string& s) { syzygy_path_ = s; } // 先存着
    void set_skill_level(int lv) { skill_level_ = std::max(0, std::min(20, lv)); } // 先存着
    int  skill_level() const { return skill_level_; }

    // ===== position =====
    void set_startpos() { pos.set_startpos(); }
    void set_fen(const std::string& fen) { pos.set_fen(fen); }

    Color side_to_move() const { return pos.side; }

    // ===== stop =====
    void stop() { search::stop(); }

    // ===== apply uci move =====
    void push_uci_move(const std::string& uciMove) {
        std::vector<Move> moves;
        movegen::generate_legal(pos, moves);

        for (Move m : moves) {
            if (move_to_uci(m) == uciMove) {
                Undo u = pos.do_move(m);
                (void)u;
                return;
            }
        }
    }

private:
    // time split for wtime/btime mode only (ms)
    static inline int compute_think_ms(
        int mytime_ms,
        int myinc_ms,
        int movestogo,
        int move_overhead_ms
    ) {
        if (mytime_ms <= 0) return 1;

        // ✅ 只在 wtime/btime 模式扣 overhead（防超时）
        int tleft = mytime_ms - std::max(0, move_overhead_ms);
        if (tleft < 1) tleft = 1;

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

        // 最少给一点，别抖
        t = std::max(t, 5);

        // 小保险
        if (t > 2) t -= 2;

        return t;
    }

public:
    // ===== go =====
    // depth: >0 表示用户强制深度；<=0 表示未给 depth（time-based）
    // movetime: >0 表示用户强制时长；<=0 表示未给 movetime（用 wtime/btime 算）
    int go(int depth,
           int movetime,
           bool infinite,
           int wtime, int btime,
           int winc, int binc,
           int movestogo)
    {
        search::Limits lim{};

        const bool depth_given    = (depth > 0);
        const bool movetime_given = (movetime > 0);

        // 1) infinite：无限思考（靠 stop 停）
        if (infinite) {
            lim.infinite = true;
            lim.movetime_ms = 0;

            // depth 给了就用；没给就 0（Search.h 内部会当成 64）
            lim.depth = depth_given ? depth : 0;

            search::Result res = search::think(pos, lim);
            return (int)res.bestMove;
        }

        // 2) 计算本步可用时间（ms）
        int t_ms = 0;

        if (movetime_given) {
            // ✅ 关键：movetime 就是“给多少用多少”，不要扣 Move Overhead
            t_ms = std::max(1, movetime);
        } else {
            // ✅ 只有 wtime/btime 才需要 overhead
            int myTime = (pos.side == WHITE ? wtime : btime);
            int myInc  = (pos.side == WHITE ? winc  : binc);

            if (myTime < 0) myTime = 0;
            if (myInc  < 0) myInc  = 0;

            t_ms = compute_think_ms(myTime, myInc, movestogo, move_overhead_ms_);
            if (t_ms < 1) t_ms = 1;
        }

        lim.infinite = false;
        lim.movetime_ms = t_ms;

        // 3) depth 策略
        // - 用户给 depth：按 depth（但仍受 movetime 限制，谁先到谁停）
        // - 用户没给 depth：lim.depth=0 -> Search.h 默认 64（不再卡 14）
        lim.depth = depth_given ? depth : 0;

        // 4) Skill Level：只在 skill<20 且用户没给 depth 时弱化
        //    ✅ 注意：即使弱化，也不改变“movetime=1000 用满”的语义 —— 只在非 movetime_given 时缩短时间
        if (!depth_given && skill_level_ < 20) {
            int capDepth = 4 + skill_level_ / 2;   // 0..19 -> 4..13
            capDepth = std::max(1, std::min(64, capDepth));
            lim.depth = capDepth;

            // ✅ 只在不是 movetime_given（也就是 clock 模式）时缩短用时
            if (!movetime_given) {
                int factor = 40 + (skill_level_ * 50) / 19; // 40..90
                lim.movetime_ms = std::max(1, (lim.movetime_ms * factor) / 100);
            }
        }

        search::Result res = search::think(pos, lim);
        return (int)res.bestMove;
    }

    // ===== move->uci =====
    std::string move_to_uci(int m) const { return move_to_uci((Move)m); }

    std::string move_to_uci(Move m) const {
        int from = from_sq(m);
        int to   = to_sq(m);

        std::string s;
        s += char('a' + file_of(from));
        s += char('1' + rank_of(from));
        s += char('a' + file_of(to));
        s += char('1' + rank_of(to));

        int promo = promo_of(m);
        if (promo) {
            char pc = 'q';
            if (promo == 1) pc = 'n';
            else if (promo == 2) pc = 'b';
            else if (promo == 3) pc = 'r';
            else if (promo == 4) pc = 'q';
            s += pc;
        }
        return s;
    }

private:
    Position pos;

    // options cache
    int  threads_ = 1;
    int  multipv_ = 1;
    bool ponder_ = false;
    int  move_overhead_ms_ = 10;
    std::string syzygy_path_;
    int  skill_level_ = 20;
};
