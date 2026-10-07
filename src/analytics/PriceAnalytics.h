#pragma once

#include <vector>
#include <unordered_map>
#include <algorithm>

#include "SetAnalytics.h"

class PriceAnalytics {
private:
    std::vector<double> prices;
    std::vector<double> volume;

public:
    PriceAnalytics(std::vector<double> prices={}, std::vector<double> volume={}) : prices(prices), volume(volume) {}

    void updatePrices(double price, double vol = 0.0) { prices.push_back(price); volume.push_back(vol); }
    void trimPrices(int maxSize) {
        if ((int)prices.size() > maxSize) { prices.erase(prices.begin()); volume.erase(volume.begin()); }
    }

    std::vector<double> returnSimpleMovingAverage(int period) { // "upstream" definition of returnRollingMovingAverage
        SetAnalytics sa(prices); // stack object, dies with the scope, nothing to free
        return sa.returnRollingMovingAverage(period);
    }

    /// @brief exponential moving average, same tail-slice convention as the SMA
    /// seeds with a simple average of the first `period` values, then applies the
    /// standard EMA formula from there
    /// @param period lookback / smoothing length
    std::vector<double> returnExponentialMovingAverage(int period) {
        if ((int)prices.size() < period * 2) return {};
        std::vector<double> requiredChunk(prices.end() - (period*2), prices.end());
        std::vector<double> emaOverTime;

        double multiplier = 2.0 / static_cast<double>(period + 1);

        double seed = 0;
        for (int i = 0; i < period; i++) { seed += requiredChunk[i]; }
        double ema = seed / static_cast<double>(period);

        for (int i = period; i < requiredChunk.size(); i++) {
            ema = (requiredChunk[i] - ema) * multiplier + ema;
            emaOverTime.push_back(ema);
        }

        return emaOverTime;
    }

    /// @brief builds a volume profile from `anchor` forward, groups volume by price
    /// level so you can see where the most trading happened
    /// @param anchor   starting index inside `prices` (everything before is ignored)
    /// @param prices   the price vec to scan
    /// @param volumeData per-bar volume, same length as prices
    /// @return [[price, volume], [price, volume], ...] feed this into returnValueArea
    std::vector<std::vector<double>> returnVolumeProfile(int anchor) { // anchor idx inside of the prices vec that gets passed
        std::unordered_map<double, double> map;
        for (int i=anchor; i<(int)prices.size(); i++) map[prices[i]] += volume[i];
        std::vector<std::vector<double>> passedPrices;
        passedPrices.reserve(map.size());
        for (auto& [px, vol] : map) passedPrices.push_back({px, vol});
        return passedPrices;
    } 

    /// @brief returns {VAL, VAH}, the price range containing 70% of total volume,
    /// expanding outward from the POC (highest-volume price level)
    /// @param volumeProfile output of returnVolumeProfile: [[price, volume], ...]
    /// @param pct           fraction of total volume to capture (default 0.70)
    /// @return {VAL, VAH} price pair, or {0,0} if the profile is empty
    std::vector<double> returnValueArea(std::vector<std::vector<double>> volumeProfile, double pct = 0.70) {
        if (volumeProfile.empty()) return {0.0, 0.0};

        // Sort by price ascending so we can walk outward by index
        std::sort(volumeProfile.begin(), volumeProfile.end(),
            [](const std::vector<double>& a, const std::vector<double>& b) { return a[0] < b[0]; });

        // Total volume + find POC (index of highest-volume level)
        double totalVol = 0.0;
        int pocIdx = 0;
        for (int i = 0; i < (int)volumeProfile.size(); i++) {
            totalVol += volumeProfile[i][1];
            if (volumeProfile[i][1] > volumeProfile[pocIdx][1]) pocIdx = i;
        }

        double targetVol = totalVol * pct;
        double captured = volumeProfile[pocIdx][1];
        int lo = pocIdx;
        int hi = pocIdx;

        // Expand whichever side adds more volume until we hit the target
        while (captured < targetVol && (lo > 0 || hi < (int)volumeProfile.size() - 1)) {
            double volBelow = (lo > 0) ? volumeProfile[lo - 1][1] : 0.0;
            double volAbove = (hi < (int)volumeProfile.size() - 1) ? volumeProfile[hi + 1][1] : 0.0;

            if (lo <= 0) {
                hi++; captured += volAbove;
            } else if (hi >= (int)volumeProfile.size() - 1) {
                lo--; captured += volBelow;
            } else if (volBelow >= volAbove) {
                lo--; captured += volBelow;
            } else {
                hi++; captured += volAbove;
            }
        }

        return {volumeProfile[lo][0], volumeProfile[hi][0]};
    }

    /// @brief anchored volume-weighted average price, running/cumulative from
    /// `anchor` forward so vwap[i] reflects every bar from anchor through i,
    /// one output value per bar in that range (parallel to prices[anchor..])
    /// @param anchor starting index inside `prices` (everything before is ignored)
    /// @return running VWAP, one value per bar from anchor to the end of prices
    std::vector<double> returnVWAP(int anchor) {
        std::vector<double> vwap;
        if (anchor < 0 || anchor >= (int)prices.size()) return vwap;
        vwap.reserve(prices.size() - anchor);

        double sumPV = 0.0, sumV = 0.0;
        for (int i = anchor; i < (int)prices.size(); i++) {
            sumPV += (double)prices[i] * volume[i];
            sumV += volume[i];
            vwap.push_back(sumV > 0.0 ? static_cast<double>(sumPV / sumV) : prices[i]);
        }
        return vwap;
    }
};