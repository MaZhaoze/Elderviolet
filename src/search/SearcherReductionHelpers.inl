    inline int quiet_futility_margin(int /*depth*/, bool /*improving*/) const { return 0; }
    inline int quiet_limit_for_depth(int /*depth*/) const { return 64; }
    inline void update_quiet_history(Color /*us*/, int /*prevFrom*/, int /*prevTo*/, int /*from*/, int /*to*/, int /*depth*/,
                                     bool /*good*/) {}
    inline int compute_lmr_reduction(int /*depth*/, int /*legalMovesSearched*/, bool /*inCheck*/, bool /*isQuiet*/,
                                     bool /*improving*/, bool /*isPv*/) const { return 0; }
