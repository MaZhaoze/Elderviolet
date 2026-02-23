    inline PVLine sanitize_pv_from_root(const Position& root, const PVLine& in, int maxLen) {
        PVLine out{};
        Position p = root;
        const int lim = std::min(maxLen, in.len);
        for (int i = 0; i < lim && out.len < 128; i++) {
            Move m = in.m[i];
            if (!is_legal_move_here(p, m, i))
                break;
            out.m[out.len++] = m;
            (void)p.do_move(m);
        }
        return out;
    }
