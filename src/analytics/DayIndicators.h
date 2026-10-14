#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "../backtestApi/dataManagement/data.h"

// numbers for one window (a day, usually). every function takes an optional keep(Bar) -> bool to
// leave bars out, e.g. everything after 15:59. prevClose is the close before the first bar, so the
// first true range sees the gap, NaN = none
struct DayStats {
    std::size_t bars = 0;
    double open = 0, high = 0, low = 0, close = 0;
    double volume = 0;
    double vwap = 0;         // typical price (h+l+c)/3 weighted by volume
    double range = 0;        // high - low
    double atr = 0;          // mean true range
    double realizedVol = 0;  // sqrt of summed squared log returns of the closes
};

template <class Keep>
DayStats dayStats(const DataWindow& w, Keep keep, double prevClose = std::numeric_limits<double>::quiet_NaN()) {
    DayStats s;
    double pv = 0, trSum = 0, rv = 0, prev = prevClose;
    for (auto b : w) {
        if (!keep(b)) continue;
        if (s.bars == 0) { s.open = b.open; s.high = b.high; s.low = b.low; }
        s.high = std::max(s.high, b.high);
        s.low = std::min(s.low, b.low);
        s.close = b.close;
        s.volume += b.volume;
        pv += (b.high + b.low + b.close) / 3.0 * b.volume;

        double tr = b.high - b.low;
        if (!std::isnan(prev)) {
            tr = std::max({tr, std::fabs(b.high - prev), std::fabs(b.low - prev)});
            if (prev > 0 && b.close > 0) { const double r = std::log(b.close / prev); rv += r * r; }
        }
        trSum += tr;
        prev = b.close;
        s.bars++;
    }
    if (s.bars) {
        s.vwap = s.volume > 0 ? pv / s.volume : s.close;
        s.range = s.high - s.low;
        s.atr = trSum / s.bars;
        s.realizedVol = std::sqrt(rv);
    }
    return s;
}

inline DayStats dayStats(const DataWindow& w) { return dayStats(w, [](const Bar&) { return true; }); }

template <class Keep>
double atr(const DataWindow& w, Keep keep, double prevClose = std::numeric_limits<double>::quiet_NaN()) {
    return dayStats(w, keep, prevClose).atr;
}
inline double atr(const DataWindow& w) { return dayStats(w).atr; }

template <class Keep>
double vwap(const DataWindow& w, Keep keep) { return dayStats(w, keep).vwap; }
inline double vwap(const DataWindow& w) { return dayStats(w).vwap; }

// true range per bar, same length as the window
inline std::vector<double> trueRange(const DataWindow& w, double prevClose = std::numeric_limits<double>::quiet_NaN()) {
    std::vector<double> out;
    out.reserve(w.size());
    double prev = prevClose;
    for (auto b : w) {
        double tr = b.high - b.low;
        if (!std::isnan(prev)) tr = std::max({tr, std::fabs(b.high - prev), std::fabs(b.low - prev)});
        out.push_back(tr);
        prev = b.close;
    }
    return out;
}
