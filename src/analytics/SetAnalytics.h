#pragma once

#include <vector>
#include <numeric>
#include <map>
#include <cmath>
#include <stdexcept>

/// @brief config function, makes setting up cleaner
/// @param tickSize self explanatory
/// @param tickValue self explanatory
class SetAnalytics {
private:
    std::vector<double> data;   

public:
    SetAnalytics(std::vector<double> data) : data(data) {}


    void updateVector(double newData) { data.push_back(newData); }
    void clearOutVector(int maxSize) { if (data.size() > maxSize) data.erase(data.begin()); }

    /// @brief rolling moving average over the tail end of `data`, uses the last
    /// period*2 values so you get `period` output points (one per point after warmup)
    /// `data` needs a length of at least period*2, only the tail gets touched
    /// @param period lookback length
    std::vector<double> returnRollingMovingAverage(int period) {
        if ((int)data.size() < period * 2) return {};
        std::vector<double> requiredChunk(data.end() - (period*2), data.end());
        std::vector<double> avgOverTime;

        double sum = 0;
        for (int i = 0; i < requiredChunk.size(); i++) {
            sum += requiredChunk[i];
            if (i >= period) {
                sum -= requiredChunk[i - period];
                avgOverTime.push_back(sum / static_cast<double>(period));
            }
        }
        return avgOverTime;
    }

    /// @brief population standard deviation over the whole set, one scalar
    /// use returnRollingStandardDeviation if you want a value per point instead
    double returnStandardDeviation() {
        if (data.size() < 2) return 0.0;

        double sum = 0.0;
        for (double v : data) sum += v;
        double mean = sum / static_cast<double>(data.size());

        double sqDiff = 0.0;
        for (double v : data) { double d = v - mean; sqDiff += d * d; }

        return static_cast<double>(std::sqrt(sqDiff / static_cast<double>(data.size())));
    }

    /// @brief rolling population standard deviation, same tail-slice convention as
    /// returnRollingMovingAverage so the outputs line up index for index with it
    /// accumulators are doubles since sumSq cancellation gets ugly on price-scale floats
    /// @param period lookback length
    std::vector<double> returnRollingStandardDeviation(int period) {
        if ((int)data.size() < period * 2) return {};
        std::vector<double> requiredChunk(data.end() - (period*2), data.end());
        std::vector<double> stdDevOverTime;

        double sum = 0.0, sumSq = 0.0;
        for (int i = 0; i < requiredChunk.size(); i++) {
            sum += requiredChunk[i];
            sumSq += static_cast<double>(requiredChunk[i]) * requiredChunk[i];
            if (i >= period) {
                sum -= requiredChunk[i - period];
                sumSq -= static_cast<double>(requiredChunk[i - period]) * requiredChunk[i - period];

                double mean = sum / static_cast<double>(period);
                double variance = sumSq / static_cast<double>(period) - mean * mean;
                if (variance < 0.0) variance = 0.0; // fp noise on a flat window
                stdDevOverTime.push_back(static_cast<double>(std::sqrt(variance)));
            }
        }
        return stdDevOverTime;
    }

    /// @brief rolling z-score, how many standard deviations each value sits from
    /// the mean of its own trailing window, same tail-slice convention as the others
    /// a flat window (zero stddev) yields 0 rather than a div by zero
    /// @param period lookback length
    std::vector<double> returnRollingZScore(int period) {
        if ((int)data.size() < period * 2) return {};
        std::vector<double> requiredChunk(data.end() - (period*2), data.end());
        std::vector<double> zOverTime;

        double sum = 0.0, sumSq = 0.0;
        for (int i = 0; i < requiredChunk.size(); i++) {
            sum += requiredChunk[i];
            sumSq += static_cast<double>(requiredChunk[i]) * requiredChunk[i];
            if (i >= period) {
                sum -= requiredChunk[i - period];
                sumSq -= static_cast<double>(requiredChunk[i - period]) * requiredChunk[i - period];

                double mean = sum / static_cast<double>(period);
                double variance = sumSq / static_cast<double>(period) - mean * mean;
                if (variance < 0.0) variance = 0.0;
                double stdDev = std::sqrt(variance);

                zOverTime.push_back(stdDev > 0.0
                    ? static_cast<double>((requiredChunk[i] - mean) / stdDev)
                    : 0.0);
            }
        }
        return zOverTime;
    }

    /// @brief pearson correlation calculation between `data` and `yTerm`, a range
    /// between -1 and 1 that measures how closely correlated two vectors are
    /// @param yTerm what to check the correlation against
    double computeCorrelation(std::vector<double>& yTerm) {
        std::vector<double> XYPairProducts;
        std::vector<double> XSquaredTerms;
        std::vector<double> YSquaredTerms;

        for (std::size_t i = 0; i < data.size(); i++) {
            XYPairProducts.push_back(data[i] * yTerm[i]);
            XSquaredTerms.push_back(data[i] * data[i]);
            YSquaredTerms.push_back(yTerm[i] * yTerm[i]);
        }

        double SUMxy = std::accumulate(XYPairProducts.begin(), XYPairProducts.end(), 0.0);
        double SUMx  = std::accumulate(data.begin(), data.end(), 0.0);
        double SUMy  = std::accumulate(yTerm.begin(), yTerm.end(), 0.0);
        double SUMx2 = std::accumulate(XSquaredTerms.begin(), XSquaredTerms.end(), 0.0);
        double SUMy2 = std::accumulate(YSquaredTerms.begin(), YSquaredTerms.end(), 0.0);

        double n = static_cast<double>(data.size());

        double firstTerm  = n * SUMxy;
        double secondTerm = SUMx * SUMy;
        double top        = firstTerm - secondTerm;

        double thirdTerm  = n * SUMx2;
        double fourthTerm = std::pow(SUMx, 2);

        double fifthTerm  = n * SUMy2;
        double sixthTerm  = std::pow(SUMy, 2);

        double bottom = std::sqrt((thirdTerm - fourthTerm) * (fifthTerm - sixthTerm));

        return bottom != 0.0 ? top / bottom : 0.0;
    }

    /// @brief loop through the series and return a new series
    /// of fisher-transformed values
    /// @param lowerBound lower bound of the series
    /// @param upperBound analagous to lowerBound
    std::vector<double> fisherTransform(double lowerBound = -1.0, double upperBound = 1.0) {
        std::vector<double> transformedSeries;
        for (int i = 0; i<data.size(); i++) {
            if (data[i] <= lowerBound || data[i] >= upperBound) {
                throw std::out_of_range("A value in the series passed in SetAnalytics.fisherTransform was outside the specified bounds");
            }
            transformedSeries.push_back(0.5 * std::log((data[i] - lowerBound) / (upperBound - data[i])));
        }
        return transformedSeries;
    }

    /// @brief returns tanh'd series, same logic as the above transform,
    /// it is however boundless
    std::vector<double> tanhTransform() {
        std::vector<double> transformedSeries;
        for (int i = 0; i<data.size(); i++) {
            transformedSeries.push_back(std::tanh(data[i]));
        }
        return transformedSeries;
    }

    /// @brief histogram-style bucketing, each value lands in the bucket
    /// [k*bucketSize, (k+1)*bucketSize), keyed by that lower edge
    /// i.e {0.5, 0.7, 1.8, 4.3} bucketed by 1 -> {0: 2, 1: 1, 4: 1}
    /// empty buckets are skipped, map keeps them sorted low to high
    /// @param bucketSize width of each bucket, must be > 0
    std::map<double, int> bucket(double bucketSize = 1.0) {
        if (bucketSize <= 0.0) {
            throw std::invalid_argument("bucketSize passed in SetAnalytics.bucket must be > 0");
        }
        std::map<double, int> buckets;
        for (double v : data) {
            buckets[std::floor(v / bucketSize) * bucketSize]++; // floor so negatives go down, not toward 0
        }
        return buckets;
    }
};