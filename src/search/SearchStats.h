#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace search {

enum MoveBucket {
    MB_TT = 0,
    MB_CAP_GOOD = 1,
    MB_QUIET_SPECIAL = 2,
    MB_QUIET = 3,
    MB_CAP_BAD = 4,
};

struct PruneStats {
    uint64_t razorPrune = 0;
    uint64_t rfpPrune = 0;
    uint64_t probCutPrune = 0;
    uint64_t quietFutility = 0;
    uint64_t quietLimit = 0;
    uint64_t capSeePrune = 0;
    uint64_t iirApplied = 0;
    uint64_t lmrApplied = 0;
    uint64_t betaCutoff = 0;

    void clear() { *this = PruneStats{}; }
};

struct SearchStats {
    static constexpr int BUCKET_N = 5;

    uint64_t timeChecks = 0;
    uint64_t makeCalls = 0;
    uint64_t makeMain = 0;
    uint64_t makeQ = 0;
    uint64_t seeCallsMain = 0;
    uint64_t seeCallsQ = 0;
    uint64_t seeFastSafe = 0;
    uint64_t pinCalc = 0;

    uint64_t rootNonFirstTried = 0;
    uint64_t rootPvsReSearch = 0;
    uint64_t rootLmrReSearch = 0;
    uint64_t rootIters = 0;
    uint64_t rootFirstBestOrCut = 0;
    uint64_t rootBestSrc[5]{};
    uint64_t aspFail = 0;

    uint64_t ttProbe = 0;
    uint64_t ttHit = 0;
    uint64_t ttCut = 0;
    uint64_t ttMoveAvail = 0;
    uint64_t ttMoveFirst = 0;

    uint64_t lmrTried = 0;
    uint64_t lmrResearched = 0;
    uint64_t lmrReducedByBucket[4]{};
    uint64_t lmrResearchedByBucket[4]{};

    uint64_t nullTried = 0;
    uint64_t nullCut = 0;
    uint64_t nullVerifyFail = 0;

    uint64_t proxyReversalAfterNull = 0;
    uint64_t proxyReversalAfterRfp = 0;
    uint64_t proxyReversalAfterRazor = 0;

    uint64_t legCalls = 0;
    uint64_t legFail = 0;
    uint64_t legQuiet = 0;
    uint64_t legCapture = 0;
    uint64_t legCheck = 0;
    uint64_t legEp = 0;
    uint64_t legKing = 0;
    uint64_t legSuspin = 0;
    uint64_t legFast = 0;
    uint64_t legFast2 = 0;

    uint64_t nodeByType[3]{};
    uint64_t legalByType[3]{};

    uint64_t bucketTry[3][BUCKET_N]{};
    uint64_t bucketFh[3][BUCKET_N]{};
    uint64_t bucketBest[3][BUCKET_N]{};
    uint64_t bucketSee[3][BUCKET_N]{};
    uint64_t bucketLeg[3][BUCKET_N]{};
    uint64_t bucketMk[3][BUCKET_N]{};

    void clear() { *this = SearchStats{}; }
};

inline std::atomic<bool> g_collect_stats{false};

inline bool collect_stats() {
    return g_collect_stats.load(std::memory_order_relaxed);
}

inline void set_collect_stats(bool on) {
    g_collect_stats.store(on, std::memory_order_relaxed);
}

} // namespace search
