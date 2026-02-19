// =========================
// Engine.h（完整替换）
// =========================
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

    ~Engine() {
        stop(); // 确保退出时 join 线程
    }

    void new_game() {
        stop();
        pos.set_startpos();
        search::clear_tt();
        last_ponder_move_.store(0, std::memory_order_relaxed);
    }

    // ===== options =====
    void set_hash(int mb) { search::set_hash_mb(mb); }

    void set_threads(int n) {
        threads_ = std::max(1, n);
        search::set_threads(threads_);
    }
    void set_multipv(int n) { multipv_ = std::max(1, n); } // 先存着（暂不实现 multipv 输出）
    void set_ponder(bool b) { ponder_ = b; }               // 先存着（UCI go ponder 仍可用）
    void set_move_overhead(int ms) { move_overhead_ms_ = std::max(0, ms); }
    int move_overhead_ms() const { return move_overhead_ms_; }
    void set_syzygy_path(const std::string& s) { syzygy_path_ = s; } // 先存着
    void set_skill_level(int lv) { skill_level_ = std::max(0, std::min(20, lv)); }
    int skill_level() const { return skill_level_; }

    // ===== position =====
    void set_startpos() { pos.set_startpos(); }
    void set_fen(const std::string& fen) { pos.set_fen(fen); }
    Color side_to_move() const { return pos.side; }

    // ===== stop =====
    void stop() {
        // 发 stop 给 search
        search::stop();

        // 若有后台线程，join
        if (searching_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> g(bg_mtx_);
            if (bg_thread_.joinable())
                bg_thread_.join();
            searching_.store(false, std::memory_order_release);
            pondering_.store(false, std::memory_order_release);
        }
    }

    // ===== UCI: ponderhit =====
    // 最小实现：停止 ponder 线程，拿到结果
    void ponderhit() {
        if (!pondering_.load(std::memory_order_acquire))
            return;
        stop(); // stop + join
        // 结果已经写入 last_best_move_/last_ponder_move_
    }

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

    // ===== getter =====
    int get_last_ponder_move() const { return last_ponder_move_.load(std::memory_order_relaxed); }

    // ===== go =====
    // depth: >0 表示用户强制深度；<=0 表示未给 depth（time-based）
    // movetime: >0 表示用户强制时长；<=0 表示未给 movetime（用 wtime/btime 算）
    // ponder: go ponder（后台无限搜，不立即输出 bestmove）
    int go(int depth, int movetime, bool infinite, int wtime, int btime, int winc, int binc, int movestogo,
           bool ponder) {
        // 如果正在后台搜，先停掉（避免重入）
        stop();

        search::Limits lim{};
        lim.depth = 0;
        lim.movetime_ms = 0;
        lim.infinite = false;

        const bool depth_given = (depth > 0);
        const bool movetime_given = (movetime > 0);

        const bool hasClock = (wtime > 0) || (btime > 0) || (winc > 0) || (binc > 0) || (movestogo > 0);

        // go ponder：当作无限思考（直到 ponderhit/stop）
        if (ponder) {
            lim.infinite = true;
            lim.movetime_ms = 0;
            lim.depth = depth_given ? depth : 0;

            start_background_search(lim); // 不输出 bestmove
            pondering_.store(true, std::memory_order_release);
            return 0; // UCI 层收到 0 时不输出 bestmove
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

        // 2) movetime 绝对优先
        if (movetime_given) {
            lim.infinite = false;
            lim.movetime_ms = std::max(1, movetime);
            lim.depth = depth_given ? depth : 0;

            search::Result r = search::think(pos, lim);
            last_ponder_move_.store((int)r.ponderMove, std::memory_order_relaxed);
            return (int)r.bestMove;
        }

        // 3) clock 模式：没给 movetime 且真的有 clock 才算时间
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
            // 4) 纯 depth / 纯 go（无 clock 无 movetime）
            lim.movetime_ms = 0;
            lim.infinite = false;
        }

        // depth 策略
        lim.depth = depth_given ? depth : 0;

        // Skill Level：只在 “用户没给 depth 且是 clock 模式” 时弱化
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

    // ===== move->uci =====
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
    // time split for wtime/btime mode only (ms)
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
        // 用当前 pos 的拷贝，不改 Engine 内部 pos
        Position pcopy = pos;

        searching_.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> g(bg_mtx_);
        bg_thread_ = std::thread([this, pcopy, lim]() mutable {
            // 后台不输出 info（避免乱序）
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
