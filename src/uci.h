// UCI protocol handling and command parsing.
#pragma once
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "Engine.h"

namespace uci {

// Parsing helpers.
static inline std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}
static inline std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    return s;
}
static inline std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

static inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok)
        out.push_back(tok);
    return out;
}

static inline int to_int_safe(const std::string& s, int def) {
    if (s.empty())
        return def;

    int sign = 1;
    size_t i = 0;
    if (s[0] == '-') {
        sign = -1;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }

    long long x = 0;
    for (; i < s.size(); i++) {
        char c = s[i];
        if (c < '0' || c > '9')
            return def;
        x = x * 10 + (c - '0');
        if (x > 2000000000LL)
            break;
    }
    x *= sign;
    if (x < -2000000000LL || x > 2000000000LL)
        return def;
    return (int)x;
}

static inline std::string to_lower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

static inline bool to_bool_safe(const std::string& s, bool def) {
    std::string x = to_lower(s);
    if (x == "true" || x == "1" || x == "yes" || x == "on")
        return true;
    if (x == "false" || x == "0" || x == "no" || x == "off")
        return false;
    return def;
}

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

// =====================
// UCI: output header
// =====================
static inline void uci_id(const Engine&) {
    std::cout << "id name Elderviolet-avx2 1.0\n";
    std::cout << "id author Magnus\n";
    std::cout << "option name Threads type spin default 1 min 1 max 256\n";
    std::cout << "option name Hash type spin default 64 min 1 max 4096\n";
    std::cout << "option name MultiPV type spin default 1 min 1 max 10\n";
    std::cout << "option name Ponder type check default false\n";
    std::cout << "option name Move Overhead type spin default 30 min 0 max 5000\n";
    std::cout << "option name SyzygyPath type string default <empty>\n";
    std::cout << "option name Skill Level type spin default 20 min 0 max 20\n";
    std::cout << "option name SearchStats type check default false\n";
    std::cout << "option name UseBook type check default true\n";
    std::cout << "option name BookDepth type spin default 16 min 0 max 128\n";
    std::cout << "option name BookFile type string default GMopenings.bin\n";
    std::cout << "uciok\n";
    std::cout.flush();
}

static inline void cmd_isready() {
    std::cout << "readyok\n";
    std::cout.flush();
}

// =====================
// UCI: command handlers
// =====================
static inline void cmd_ucinewgame(Engine& engine) {
    engine.new_game();
}

static inline void cmd_setoption(const std::vector<std::string>& tokens, Engine& engine) {
    std::string name;
    std::string value;

    auto itName = std::find(tokens.begin(), tokens.end(), "name");
    if (itName != tokens.end() && (itName + 1) != tokens.end()) {
        auto it = itName + 1;
        while (it != tokens.end() && *it != "value") {
            if (!name.empty())
                name += " ";
            name += *it;
            ++it;
        }
    }

    auto itVal = std::find(tokens.begin(), tokens.end(), "value");
    if (itVal != tokens.end() && (itVal + 1) != tokens.end()) {
        for (auto it = itVal + 1; it != tokens.end(); ++it) {
            if (!value.empty())
                value += " ";
            value += *it;
        }
    }

    const std::string lname = to_lower(name);

    if (lname == "hash") {
        int mb = to_int_safe(value, 64);
        mb = clampi(mb, 1, 4096);
        engine.set_hash(mb);
        return;
    }

    if (lname == "threads") {
        int n = to_int_safe(value, 1);
        n = clampi(n, 1, 256);
        engine.set_threads(n);
        return;
    }

    if (lname == "multipv") {
        int n = to_int_safe(value, 1);
        n = clampi(n, 1, 10);
        engine.set_multipv(n);
        return;
    }

    if (lname == "ponder") {
        bool b = to_bool_safe(value, false);
        engine.set_ponder(b);
        return;
    }

    if (lname == "move overhead") {
        int ms = to_int_safe(value, 30);
        ms = clampi(ms, 0, 5000);
        engine.set_move_overhead(ms);
        return;
    }

    if (lname == "syzygypath") {
        engine.set_syzygy_path(value);
        return;
    }

    if (lname == "skill level") {
        int lv = to_int_safe(value, 20);
        lv = clampi(lv, 0, 20);
        engine.set_skill_level(lv);
        return;
    }

    if (lname == "searchstats") {
        bool b = to_bool_safe(value, false);
        engine.set_search_stats(b);
        return;
    }

    if (lname == "usebook") {
        bool b = to_bool_safe(value, true);
        engine.set_use_book(b);
        return;
    }

    if (lname == "bookdepth") {
        int ply = to_int_safe(value, 16);
        ply = clampi(ply, 0, 128);
        engine.set_book_depth(ply);
        return;
    }

    if (lname == "bookfile") {
        engine.set_book_file(value);
        return;
    }
}

static inline void cmd_position(const std::vector<std::string>& tokens, Engine& engine) {
    if (tokens.size() < 2)
        return;

    int idx = 1;
    if (tokens[idx] == "startpos") {
        engine.set_startpos();
        idx++;
    } else if (tokens[idx] == "fen") {
        idx++;
        std::string fen;
        while (idx < (int)tokens.size() && tokens[idx] != "moves") {
            if (!fen.empty())
                fen += " ";
            fen += tokens[idx];
            idx++;
        }
        engine.set_fen(fen);
    } else {
        return;
    }

    if (idx < (int)tokens.size() && tokens[idx] == "moves") {
        idx++;
        for (; idx < (int)tokens.size(); idx++) {
            engine.push_uci_move(tokens[idx]);
        }
    }
}

static inline void cmd_go(const std::vector<std::string>& tokens, Engine& engine) {
    bool provided_depth = false;
    bool provided_movetime = false;
    bool infinite = false;

    bool provided_wtime = false, provided_btime = false;
    bool provided_winc = false, provided_binc = false;
    bool provided_mtg = false;

    bool ponder = false;

    int depth = 0;
    int movetime = 0;

    int wtime = -1, btime = -1;
    int winc = -1, binc = -1;
    int movestogo = 0;

    for (int i = 1; i < (int)tokens.size(); i++) {
        const std::string& t = tokens[i];

        if (t == "ponder") {
            ponder = true;
        } else if (t == "infinite") {
            infinite = true;
        } else if (t == "depth" && i + 1 < (int)tokens.size()) {
            depth = to_int_safe(tokens[++i], 0);
            provided_depth = true;
        } else if (t == "movetime" && i + 1 < (int)tokens.size()) {
            movetime = to_int_safe(tokens[++i], 0);
            provided_movetime = true;
        } else if (t == "wtime" && i + 1 < (int)tokens.size()) {
            wtime = to_int_safe(tokens[++i], -1);
            provided_wtime = true;
        } else if (t == "btime" && i + 1 < (int)tokens.size()) {
            btime = to_int_safe(tokens[++i], -1);
            provided_btime = true;
        } else if (t == "winc" && i + 1 < (int)tokens.size()) {
            winc = to_int_safe(tokens[++i], -1);
            provided_winc = true;
        } else if (t == "binc" && i + 1 < (int)tokens.size()) {
            binc = to_int_safe(tokens[++i], -1);
            provided_binc = true;
        } else if (t == "movestogo" && i + 1 < (int)tokens.size()) {
            movestogo = to_int_safe(tokens[++i], 0);
            provided_mtg = true;
        }
    }

    const bool hasClock = provided_wtime || provided_btime || provided_winc || provided_binc || provided_mtg;

    // If no limits are provided, fall back to a short fixed movetime.
    if (!provided_depth && !provided_movetime && !infinite && !hasClock && !ponder) {
        movetime = 1000;
        provided_movetime = true;
    }

    int depth_arg = provided_depth ? depth : 0;
    int movetime_arg = provided_movetime ? movetime : 0;

    int wtime_arg = provided_wtime ? wtime : -1;
    int btime_arg = provided_btime ? btime : -1;
    int winc_arg = provided_winc ? winc : -1;
    int binc_arg = provided_binc ? binc : -1;
    int mtg_arg = provided_mtg ? movestogo : 0;

    int bestMove =
        engine.go(depth_arg, movetime_arg, infinite, wtime_arg, btime_arg, winc_arg, binc_arg, mtg_arg, ponder);

    // Ponder searches do not output bestmove until ponderhit.
    if (ponder) {
        return;
    }

    int ponderMove = engine.get_last_ponder_move();

    std::cout << "bestmove " << engine.move_to_uci(bestMove);
    if (ponderMove) {
        std::cout << " ponder " << engine.move_to_uci(ponderMove);
    }
    std::cout << "\n";
    std::cout.flush();
}

// Main UCI input loop.
static inline void loop(Engine& engine) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        auto tokens = split(line);
        const std::string& cmd = tokens[0];

        if (cmd == "uci") {
            uci_id(engine);
        } else if (cmd == "isready") {
            cmd_isready();
        } else if (cmd == "ucinewgame") {
            cmd_ucinewgame(engine);
        } else if (cmd == "setoption") {
            cmd_setoption(tokens, engine);
        } else if (cmd == "position") {
            cmd_position(tokens, engine);
        } else if (cmd == "go") {
            cmd_go(tokens, engine);
        } else if (cmd == "ponderhit") {
            engine.ponderhit();
            // Many GUIs will send a fresh "go" after ponderhit.
        } else if (cmd == "stop") {
            engine.stop();
        } else if (cmd == "quit") {
            engine.stop();
            break;
        } else if (cmd == "ping") {
            std::cout << "pong\n";
            std::cout.flush();
        }
    }
}

} // namespace uci
