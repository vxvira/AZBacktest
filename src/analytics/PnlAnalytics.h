#pragma once

#include <vector>
#include <random>
#include <algorithm>

#include "../backtestApi/tradeManagement/tradeData.h"

// TODO - Use an OOP approach for this, like SetAnalytics / PriceAnalytics

/// @brief fraction of trades that were winners (0.0-1.0), returns 0 if no trades
double returnWinrate() {
    if (trades.empty()) return 0.0;

    int cumTrades = 0;
    int cumWins = 0;

    for (const auto& t : trades) {
        ++cumTrades;
        if (t.win) ++cumWins;
    }

    return static_cast<double>(cumWins) / static_cast<double>(cumTrades);
}

/// @brief total profit across all closed trades, in points
double returnCumProfit() {
    if (trades.empty()) return 0.0;

    double cumProfit = 0;

    for (auto& t : trades) { cumProfit += t.profit; }
    return cumProfit;
}

/// @brief equity curve bucketed into fixed-width time bars, each bar holds the
/// last equity value that fell inside it; empty bars carry forward
/// @param bucketMins width of each bar in minutes (default 1)
std::vector<double> returnProfitOverTime(int bucketMins = 1) {
    if (equityCurve.empty() || bucketMins <= 0) return std::vector<double>{};

    const long long bucketSec = static_cast<long long>(bucketMins) * 60LL;
    const long long firstBucket = equityCurve.front().first / bucketSec;
    const long long lastBucket  = equityCurve.back().first  / bucketSec;
    const size_t n = static_cast<size_t>(lastBucket - firstBucket + 1);

    // For each bucket, take the last equity sample that falls inside it.
    // Empty buckets carry the previous bucket's equity forward.
    std::vector<double> profitOverTime(n, 0.0);
    double lastEq = 0.0;
    size_t sampleIdx = 0;
    for (size_t b = 0; b < n; ++b) {
        long long bucketMax = (firstBucket + static_cast<long long>(b) + 1) * bucketSec - 1;
        while (sampleIdx < equityCurve.size() && equityCurve[sampleIdx].first <= bucketMax) {
            lastEq = equityCurve[sampleIdx].second;
            ++sampleIdx;
        }
        profitOverTime[b] = lastEq;
    }
    return profitOverTime;
}

/// @brief running average profit per trade, one value per closed trade, so you
/// can see if your edge is improving or degrading over time
std::vector<double> returnAverageProfitOverTime() {
    if (trades.empty()) return std::vector<double>{};

    std::vector<double> avgOverTime;
    avgOverTime.reserve(trades.size());
    double sum = 0.0;
    for (size_t i = 0; i < trades.size(); ++i) {
        sum += trades[i].profit;
        avgOverTime.push_back(sum / static_cast<double>(i + 1));
    }
    return avgOverTime;
}

/// @brief cumulative profit indexed per trade
std::vector<double> returnCumProfitPerTrade() {
    std::vector<double> curve;
    curve.reserve(trades.size());
    double cum = 0.0;
    for (auto& t : trades) {
        cum += t.profit;
        curve.push_back(cum);
    }
    return curve;
}

/// @brief cumulative profit bucketed by time period, matches returnMonteCarlo bucketing
/// @param bucketSecs bucket width in seconds (86400 = daily)
std::vector<double> returnCumProfitBucketed(int bucketSecs = 86400) {
    if (trades.empty() || bucketSecs <= 0) return {};
    long long bsec = (long long)bucketSecs;
    long long curBucket = trades[0].closeEpochSec / bsec;
    double bucketPnl = 0.0;
    double cum = 0.0;
    std::vector<double> curve;
    for (auto& t : trades) {
        long long tb = t.closeEpochSec / bsec;
        if (tb != curBucket) {
            cum += bucketPnl;
            curve.push_back(cum);
            for (long long gap = curBucket + 1; gap < tb; gap++) curve.push_back(cum);
            curBucket = tb;
            bucketPnl = 0.0;
        }
        bucketPnl += t.profit;
    }
    cum += bucketPnl;
    curve.push_back(cum);
    return curve;
}

/// @brief downsample a vector to at most maxPts points using largest-triangle-three-buckets-ish
/// keeps first and last, picks representative points in between
std::vector<double> downsample(const std::vector<double>& src, int maxPts) {
    int n = (int)src.size();
    if (n <= maxPts) return src;
    std::vector<double> out;
    out.reserve(maxPts);
    for (int i = 0; i < maxPts; i++) {
        int idx = (int)((long long)i * (n - 1) / (maxPts - 1));
        out.push_back(src[idx]);
    }
    return out;
}

/// @brief average P&L of trades matching `pred`, this is the building block for
/// returnAverageWinSize / returnAverageLossSize, but you can pass any filter
/// @param pred a callable that takes a tradeData and returns true for trades to include
template <typename Predicate>
double returnExpectancy(Predicate pred) {
    if (trades.empty()) return 0.0;

    double cumPnL = 0;
    int count = 0;
    for (const auto& t : trades) {
        if (pred(t)) {
            cumPnL += t.profit;
            ++count;
        }
    }
    if (count == 0) return 0.0;
    return cumPnL / static_cast<double>(count);
}

/// @brief average profit of winning trades (in pts)
double returnAverageWinSize() {
    return returnExpectancy([](const tradeData& t) { return t.win; });
}

/// @brief average loss of losing trades (in pts, will be negative)
double returnAverageLossSize() {
    return returnExpectancy([](const tradeData& t) { return !t.win; });
}

/// @brief average P&L across all trades (in pts)
double returnAvgPnl() {
    return returnExpectancy([](const tradeData&) { return true; });
}

/// @brief trades per calendar day based on first/last trade close timestamps
double returnTradesPerDay() {
    if (trades.size() < 2) return 0.0;
    long long first = trades.front().closeEpochSec;
    long long last  = trades.back().closeEpochSec;
    if (last <= first) return 0.0;
    double days = (last - first) / 86400.0;
    if (days < 0.001) return 0.0;
    return (double)trades.size() / days;
}

// mc stuff only for equity curve, uses stationary bootstrap (Politis & Romano 1994)

/// @brief stationary bootstrap MC, resamples blocks of consecutive trades with
/// geometrically distributed block lengths to preserve serial dependence
/// @param sims how many simulations to run
/// @param avgBlockLen expected block length (controls how much dependence is kept)
/// @param bucketSecs if > 0, aggregate trade PnLs into buckets of this width before
///        bootstrapping (86400 = daily). output paths are per-bucket instead of per-trade
/// @param seed RNG seed, fixed by default so results are reproducible
std::vector<std::vector<double>> returnMonteCarlo(int sims, int avgBlockLen = 5,
    int bucketSecs = 0, unsigned seed = 42) {
    if (trades.empty() || sims <= 0 || avgBlockLen <= 0) return {}; // geometric dist needs p in (0, 1]

    // build the PnL series to bootstrap
    std::vector<double> pnls;
    if (bucketSecs > 0) {
        // bucket trades by time period
        long long bsec = (long long)bucketSecs;
        long long curBucket = trades[0].closeEpochSec / bsec;
        double bucketPnl = 0.0;
        for (auto& t : trades) {
            long long tb = t.closeEpochSec / bsec;
            if (tb != curBucket) {
                pnls.push_back(bucketPnl);
                // fill empty buckets with 0
                for (long long gap = curBucket + 1; gap < tb; gap++) pnls.push_back(0.0);
                curBucket = tb;
                bucketPnl = 0.0;
            }
            bucketPnl += t.profit;
        }
        pnls.push_back(bucketPnl);
    } else {
        pnls.reserve(trades.size());
        for (auto& t : trades) pnls.push_back(t.profit);
    }

    int n = (int)pnls.size();
    std::mt19937 rng(seed);
    std::geometric_distribution<int> blockDist(1.0 / avgBlockLen);
    std::uniform_int_distribution<int> startDist(0, n - 1);

    std::vector<std::vector<double>> paths;
    paths.reserve(sims);

    for (int s = 0; s < sims; s++) {
        std::vector<double> curve(n);
        double cum = 0;
        int i = 0;
        while (i < n) {
            int start = startDist(rng);
            int len = blockDist(rng) + 1;
            for (int b = 0; b < len && i < n; b++, i++) {
                cum += pnls[(start + b) % n];
                curve[i] = cum;
            }
        }
        paths.push_back(std::move(curve));
    }
    return paths;
}

/// @brief extract percentile lines from MC paths. for each trade step, sorts
/// across all sims and picks the value at each percentile
/// @param mcPaths output of returnMonteCarlo
/// @param percentiles list of percentiles 0-100 (e.g. {5, 50, 95})
/// @return one column per percentile, each column has trades.size() values
std::vector<std::vector<double>> returnPercentilePaths(
    const std::vector<std::vector<double>>& mcPaths,
    std::vector<int> percentiles) {

    if (mcPaths.empty() || percentiles.empty()) return {};
    int steps = (int)mcPaths[0].size();
    int nSims = (int)mcPaths.size();

    std::vector<std::vector<double>> result(percentiles.size());
    for (auto& r : result) r.resize(steps);

    std::vector<double> col(nSims);
    for (int step = 0; step < steps; step++) {
        for (int s = 0; s < nSims; s++) col[s] = mcPaths[s][step];
        std::sort(col.begin(), col.end());

        for (int p = 0; p < (int)percentiles.size(); p++) {
            int idx = (int)((percentiles[p] / 100.0) * (nSims - 1));
            if (idx >= nSims) idx = nSims - 1;
            result[p][step] = col[idx];
        }
    }
    return result;
}
