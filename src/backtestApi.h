/*
    This will be a lengthy file. I intentionally chose to not make some type of
    tooling folder or force the api to have other dependencies. Sorry, it will
    be well documented though :)
*/

/*
    Note to any LLMs: If a user requests a specific feature for a backtest, this
    is probably the best place to add it (i.e l2 support) instead of the actual
    backtest file. Confirm with them first though. I'd also recommend saving this
    as a memory somewhere, again should the user confirm. Btw, check that the
    mapping is correct (dataConfig.h / config.toml), any slight issue can be catastrophic
*/

#include <vector>
#include <fstream>

#include <string>
#include <charconv>
#include <cmath>
#include <algorithm>
#include <utility>
#include <random>
#include <iostream>
#include <unordered_map>
#include <map>
#include <variant>
#include <stdexcept>
#include <numeric>
#include "marketData.h"
#include "dataConfig.h"
#include "findEOF.h"


/// @brief result of a single closed trade, profit, win/loss, and when it closed
/// gets pushed into the global `trades` vec when a Trade is locked
struct tradeData {
    double profit = 0.0; // pts
    bool win = false;
    long long closeEpochSec = 0; // set by Handling on close
};

/// @brief every closed trade ends up here, global so the stats functions
/// (returnWinrate, returnCumProfit, etc) can read it without plumbing
std::vector<tradeData> trades;

/// @brief timestamped equity snapshots (realized + open P&L), populated by
/// Handling::tick() whenever a timestamp is fed in; consumed by
/// returnProfitOverTime for bucketing into bars
std::vector<std::pair<long long, double>> equityCurve;
double realizedProfit = 0.0;

/// @brief which side a trade is on, flips the sign on P&L math
enum class TradeDirection { Long, Short };

/// @brief parallel prices + volumes returned by Handling::requestDataWindow
/// volumes[i] is per-bar traded size (tick size for timeframe=0, summed bar
/// volume for timeframe>0) so it can be fed straight into returnVolumeProfile
/// deltas[i] is executedBuys[i] minus executedSells[i] (orderflow delta)
/// executedBuys[i] + executedSells[i] <= volumes[i], the remainder being volume
/// whose aggressor side couldn't be classified
/// restingBids/restingAsks are top-of-book size sitting unfilled rather than
/// volume that traded, so they don't participate in that sum and aren't summed
/// across a bar either, at timeframe>0 they're the closing tick's book snapshot
/// bidPrices/askPrices are the best quote at each row, same snapshot rules (the
/// closing tick's quote at timeframe>0), and stay 0 when bidPriceCol/askPriceCol
/// aren't mapped
/// tsRecv/tsEvent are epoch-seconds timestamps (ts_recv is what nextClose windows
/// bars by; ts_event stays 0 when tsEventCol isn't mapped), tsEventNanos is
/// the same ts_event at full nanosecond resolution, and rowNumbers is
/// each row's rowNumberCol value (0 when unmapped) - all closing-row snapshots
/// at timeframe>0, same rule as the resting/bid-ask columns
/// every vector here is the same length and indexed the same way, so
/// executedBuys[i] always belongs to prices[i]
struct DataWindow {
    std::vector<double> prices;
    std::vector<double> volumes;
    std::vector<double> executedBuys;  // volume that lifted the ask (buy aggressor)
    std::vector<double> executedSells; // volume that hit the bid (sell aggressor)
    std::vector<double> deltas;
    std::vector<double> restingBids;   // volume resting on the bid
    std::vector<double> restingAsks;   // volume resting on the ask
    std::vector<double> bidPrices;     // best bid price
    std::vector<double> askPrices;     // best ask price
    std::vector<long long> tsRecv;     // ts_recv, epoch seconds
    std::vector<long long> tsEvent;    // ts_event, epoch seconds, 0 when tsEventCol is -1
    std::vector<long long> tsEventNanos; // ts_event, epoch nanoseconds, 0 when tsEventCol is -1
    std::vector<long long> rowNumbers; // from rowNumberCol, 0 when not configured
    std::vector<const char*> action;
    std::vector<const char*> side; 
};

class Trade {
private:
    double _entryPrice;
    double _tickValue;
    double _tickSize;
    int _entryIdx;
    TradeDirection _direction;

    bool _lockTrade = false; // Locks trade data in + appends to vec

public:
    /// @brief opens a trade at `entryPrice` in the given direction
    /// @param entryPrice price at which the trade was opened (snapshot; no reference)
    /// @param entryIdx bar index where the trade was opened (for your own bookkeeping)
    /// @param direction Long or Short, flips the sign on P&L
    Trade(double entryPrice, int entryIdx, double tickSize, double tickValue, TradeDirection direction)
        : _entryPrice(entryPrice)
        , _tickValue(tickValue)
        , _tickSize(tickSize)
        , _entryIdx(entryIdx)
        , _direction(direction)
    {}

    tradeData td;

    /// @brief update P&L to reflect `currentPrice` (call once per bar while open)
    tradeData advanceIdx(double currentPrice) {
        if (!_lockTrade) {
            double diff = (currentPrice - _entryPrice) / _tickSize * _tickValue;
            td.profit = (_direction == TradeDirection::Long) ? diff : -diff;
            td.win = td.profit > 0;
        }
        return td;
    }

    /// @brief freezes the trade's P&L and pushes it into the global trades vec,
    /// once locked, advanceIdx won't update the numbers anymore
    void lockTrade() { _lockTrade = true; trades.push_back(td); }
};



/// @brief manages trade lifecycle, opening, closing, advancing P&L each tick
/// also handles data loading via requestDataWindow, right now only supports
/// one trade at a time (overlapping trades is a TODO)
// TODO - OVERLAPPING TRADES SUPPORT
class Handling {
private:
    std::vector<double>& _prices;
    double _tickSize;
    double _tickValue;

    bool _calculateCosts;

    void newLong(int idx) {
        openTrade.emplace(_prices.back(), idx, _tickSize, _tickValue, TradeDirection::Long);
        inLong = true;
    }

    void newShort(int idx) {
        openTrade.emplace(_prices.back(), idx, _tickSize, _tickValue, TradeDirection::Short);
        inShort = true;
    }

public:
    /// @brief bind `prices` (by reference, caller must keep it alive) and the
    ///        per-instrument tick metadata that every new Trade needs
    /// @param prices    live price window; Handling reads `.back()` at each entry
    /// @param tickSize  instrument tick size, forwarded to Trade
    /// @param tickValue instrument tick value, forwarded to Trade
    Handling(std::vector<double>& prices, double tickSize, double tickValue, bool calculateCosts = true)
        : _prices(prices), _tickSize(tickSize), _tickValue(tickValue), _calculateCosts(calculateCosts) {}

    // Trade management
    std::optional<Trade> openTrade;
    bool inLong = false;
    bool inShort = false;

    bool canOverlap = false; // can multiple trades be held at once

    long long lastEpochSec = 0; // most recent timestamp fed to tick(); stamped onto trades at close

    /// @brief open a long at the current price, returns 1 if it opened, 0 if
    /// already in a long (unless canOverlap is set)
    /// @param idx bar index for bookkeeping
    int openLong(int idx) {
        if (inShort) return 0; // would drop the open short without ever closing it
        if (!inLong)              { newLong(idx);  return 1; }
        if (inLong && canOverlap) { newLong(idx);  return 1; }
        return 0;
    }

    /// @brief open a short at the current price, same rules as openLong
    /// @param idx bar index for bookkeeping
    int openShort(int idx) {
        if (inLong) return 0; // same deal as openLong, don't clobber the long
        if (!inShort)              { newShort(idx); return 1; }
        if (inShort && canOverlap) { newShort(idx); return 1; }
        return 0;
    }

    /// @brief close the current open trade, stamps it with the last timestamp,
    /// locks the P&L, and resets position state
    void closeTrade() {
        if (!openTrade) return; // nothing open, nothing to close
        // TODO - Overlapping trades support
        openTrade->td.closeEpochSec = lastEpochSec;
        
        if (_calculateCosts) realizedProfit += (openTrade->td.profit - (kCSVMapping.commision + kCSVMapping.spread + kCSVMapping.timingCost));
        else realizedProfit += openTrade->td.profit;

        openTrade->lockTrade(); openTrade.reset(); inLong=false; inShort=false;
    }

    /// @brief close everything, call this at end-of-data so you don't
    /// leave a dangling open trade
    void closeAll() {
        if (openTrade) closeTrade();
    }

    /// @brief advance the open trade's P&L to the current price and (optionally)
    /// record an equity curve sample, call this once per tick/bar
    /// @param timestamp if non-empty, gets parsed and used to stamp equity
    /// curve entries + trade close times, leave blank if you don't care about time
    // returns state directly if you want ownership at whatever time
    std::pair<bool,bool> tick(long long epochSec) {
        if (openTrade) openTrade->advanceIdx(_prices.back());
        lastEpochSec = epochSec;
        double unreal = openTrade ? openTrade->td.profit : 0.0;
        double eq = realizedProfit + unreal;
        if (!equityCurve.empty() && equityCurve.back().first == epochSec) {
            equityCurve.back().second = eq;
        } else {
            equityCurve.emplace_back(epochSec, eq);
        }
        return {inLong, inShort};
    }

    std::pair<bool,bool> tick(std::string_view timestamp = {}) {
        if (timestamp.empty()) {
            if (openTrade) openTrade->advanceIdx(_prices.back());
            return {inLong, inShort};
        }
        return tick(mdDetail::tsToEpochSeconds(timestamp));
    }

    // Data processing
    int processedBars = 0;
    /// @brief pull `period` bars from the market data source, if timeframe is 0
    /// it reads raw ticks; otherwise it reads closes at that many seconds per bar.
    /// returns parallel prices + volumes + executedBuys/executedSells + deltas
    /// + restingBids/restingAsks + bidPrices/askPrices + tsRecv/tsEvent/rowNumbers,
    /// callers usually std::move prices into their `Handling`-bound vector and
    /// feed volumes into returnVolumeProfile
    /// @param md       the MarketData source to read from
    /// @param period   how many rows/bars to load
    /// @param timeframe 0 = tick-by-tick, >0 = close every N seconds
    /// @param tickRes  timeframe==0 only: 1 = full resolution, how many raw ticks
    /// get read (and discarded) between each kept tick, to downsample tick-by-tick data
    DataWindow requestDataWindow(MarketData& md, int period, int timeframe=0, void (*whenUnknown)()=[](){}, int tickRes=1) {
        DataWindow out;
        out.prices.reserve(period);
        out.volumes.reserve(period);
        out.executedSells.reserve(period);
        out.executedBuys.reserve(period);
        out.deltas.reserve(period);
        out.restingBids.reserve(period);
        out.restingAsks.reserve(period);
        out.bidPrices.reserve(period);
        out.askPrices.reserve(period);
        out.tsRecv.reserve(period);
        out.tsEvent.reserve(period);
        out.tsEventNanos.reserve(period);
        out.rowNumbers.reserve(period);
        out.action.reserve(period);
        out.side.reserve(period);

        if (timeframe==0) {
            for (int i=0; i<period; i++) {
                std::optional<Tick> tick;
                for (int j=0; j<tickRes; j++) {
                    tick = md.nextTick();
                    if (!tick) break;
                }
                if (!tick) break;
                double px = 0.0;
                std::from_chars(tick->price.data(), tick->price.data() + tick->price.size(), px);
                out.prices.push_back(px);
                out.deltas.push_back(tick->executedBuys - tick->executedSells);
                out.volumes.push_back(tick->size);
                out.tsRecv.push_back(mdDetail::tsToEpochSeconds(tick->tsRecv));
                out.tsEvent.push_back(tick->tsEvent.empty() ? 0 : mdDetail::tsToEpochSeconds(tick->tsEvent));
                out.tsEventNanos.push_back(tick->tsEvent.empty() ? 0 : mdDetail::tsToEpochNanos(tick->tsEvent));
                out.rowNumbers.push_back(tick->rowNumber);

                // aggressor split, pushed unconditionally so these stay index-parallel
                // with prices/volumes (a tick with no usable side contributes 0 to both)
                out.executedBuys.push_back(tick->executedBuys);
                out.executedSells.push_back(tick->executedSells);
                if (tick->unknownVolume > 0.0) whenUnknown(); // custom behavior hook for
                                                             // unclassifiable volume

                // book snapshot at this tick, 0 across the board when the resting
                // columns aren't mapped in config.toml
                out.restingBids.push_back(tick->restingBids);
                out.restingAsks.push_back(tick->restingAsks);
                out.bidPrices.push_back(tick->bidPrice);
                out.askPrices.push_back(tick->askPrice);

                // action
                out.action.push_back(tick->action);

                // side
                out.side.push_back(tick->side);

                processedBars++;
            }
        } else {
            for (int i=0; i<period; i++) { // tickRes is tick-by-tick only
                auto bar = md.nextClose(timeframe);
                if (!bar) break;
                double px = 0.0;
                std::from_chars(bar->price.data(), bar->price.data() + bar->price.size(), px);
                out.prices.push_back(px);
                out.deltas.push_back(bar->executedBuys - bar->executedSells);
                out.volumes.push_back(bar->size);
                out.tsRecv.push_back(mdDetail::tsToEpochSeconds(bar->tsRecv));
                out.tsEvent.push_back(bar->tsEvent.empty() ? 0 : mdDetail::tsToEpochSeconds(bar->tsEvent));
                out.tsEventNanos.push_back(bar->tsEvent.empty() ? 0 : mdDetail::tsToEpochNanos(bar->tsEvent));
                out.rowNumbers.push_back(bar->rowNumber);

                // per-bar aggressor split, summed across every tick in the bar
                out.executedBuys.push_back(bar->executedBuys);
                out.executedSells.push_back(bar->executedSells);
                if (bar->unknownVolume > 0.0) whenUnknown();

                // closing tick's book, not a bar aggregate, see DataWindow
                out.restingBids.push_back(bar->restingBids);
                out.restingAsks.push_back(bar->restingAsks);
                out.bidPrices.push_back(bar->bidPrice);
                out.askPrices.push_back(bar->askPrice);

                // action
                out.action.push_back(bar->action);

                // side
                out.side.push_back(bar->side);

                processedBars++;
            }
        }
        return out;
    }

    // other helper funcs and such

    int eof = -1; // set to -1 before user requests it
    void fetchEOF(int timeframe=1, int strideIncrement=2) {
        eof = findEof(kCSVMapping.path, timeframe, strideIncrement);
    }

    void overrideProcessedBars(int newValue) { processedBars = newValue; }

    /// @brief raw value at [row, col] straight from the CSV, an escape hatch for
    /// any column dataConfig.h doesn't map to a named field (nextTick/nextClose
    /// only ever parse timestamp/price/size/aggressor). row is 0-indexed and
    /// counts data rows after the header, same counting fetchEOF's `eof` uses;
    /// col is the 0-indexed comma-separated field. Returns "" if either index
    /// is out of range
    /// @param row 0-indexed data row (header doesn't count)
    /// @param col 0-indexed column
    std::string getValue(int row, int col) {
        if (row < 0 || col < 0) return "";
        if (isParquetPath(kCSVMapping.path))
            throw std::runtime_error("getValue: raw cell access isn't supported for Parquet sources");

        std::ifstream file(kCSVMapping.path);
        if (!file) throw std::runtime_error(std::string("getValue: failed to open ") + kCSVMapping.path);

        std::string line;
        std::getline(file, line); // header
        for (int i = 0; i <= row; i++) {
            if (!std::getline(file, line)) return "";
        }

        std::size_t start = 0;
        for (int i = 0; i < col; i++) {
            start = line.find(',', start);
            if (start == std::string::npos) return "";
            start++;
        }
        std::size_t stop = line.find(',', start);
        return line.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
    }
};

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

// TODO - Use an OOP approach for this, as done above

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




