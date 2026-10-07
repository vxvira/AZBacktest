#pragma once

#include <vector>
#include <fstream>
#include <string>
#include <string_view>
#include <charconv>
#include <optional>
#include <utility>
#include <stdexcept>
#include "dataManagement/data.h"
#include "tradeManagement/tradesApi.h"
#include "../marketData.h"
#include "../findEOF.h"
#include "../../dataConfig.h"
#include "../analytics/SetAnalytics.h"
#include "../analytics/PriceAnalytics.h"
#include "../analytics/PnlAnalytics.h"

/// @brief data handling api; wrapper around `requestDataWindow`. 
class DataApi {
private:
    std::vector<double>& _prices;
    double _tickSize;
    double _tickValue;

    public:
    /// @brief bind `prices` (by reference, caller must keep it alive) and the
    ///        per-instrument tick metadata that every new Trade needs
    /// @param prices    live price window the strategy feeds
    /// @param tickSize  instrument tick size, forwarded to Trade
    /// @param tickValue instrument tick value, forwarded to Trade
    DataApi(std::vector<double>& prices, double tickSize, double tickValue)
        : _prices(prices), _tickSize(tickSize), _tickValue(tickValue) {}

    // Data processing
    int processedBars = 0;
    /// @brief pull `period` bars from the market data source, if timeframe is 0
    /// it reads raw ticks; otherwise it reads closes at that many seconds per bar.
    /// returns parallel prices + volumes + executedBuys/executedSells + deltas
    /// + restingBids/restingAsks + bidPrices/askPrices + tsRecv/tsEvent/rowNumbers,
    /// callers usually std::move prices into their `TradeApi`-bound vector and
    /// feed volumes into returnVolumeProfile
    /// @param md       the MarketData source to read from
    /// @param period   how many rows/bars to load
    /// @param timeframe 0 = tick-by-tick, >0 = close every N seconds
    /// @param tickRes  timeframe==0 only: 1 = full resolution, how many raw ticks
    /// get read (and discarded) between each kept tick, to downsample tick-by-tick data
    DataWindow requestDataWindow(MarketData& md, int period, int timeframe=0, void (*whenUnknown)()=[](){}, int tickRes=1) {
        DataWindow out;
        out.prices.reserve(period);
        out.opens.reserve(period);
        out.highs.reserve(period);
        out.lows.reserve(period);
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
                out.opens.push_back(px);
                out.highs.push_back(px);
                out.lows.push_back(px);
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
                out.opens.push_back(bar->open);
                out.highs.push_back(bar->high);
                out.lows.push_back(bar->low);
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

class TradeApi {
    private:
    std::vector<double>& _prices;
    double _tickSize;
    double _tickValue;

    bool _calculateCosts;

    void newLong(int idx) {
        openTrade.emplace(_prices.back(), idx, _tickSize, _tickValue, TradesInfo::TradeDirection::Long);
        inLong = true;
    }

    void newShort(int idx) {
        openTrade.emplace(_prices.back(), idx, _tickSize, _tickValue, TradesInfo::TradeDirection::Short);
        inShort = true;
    }

public:
    /// @brief bind `prices` (by reference, caller must keep it alive) and the
    ///        per-instrument tick metadata that every new Trade needs
    /// @param prices    live price window; TradeApi reads `.back()` at each entry
    /// @param tickSize  instrument tick size, forwarded to Trade
    /// @param tickValue instrument tick value, forwarded to Trade
    TradeApi(std::vector<double>& prices, double tickSize, double tickValue, bool calculateCosts = true)
        : _prices(prices), _tickSize(tickSize), _tickValue(tickValue), _calculateCosts(calculateCosts) {}

    // Trade management
    std::optional<Trade> openTrade;
    TradesInfo info; // closed trades + equity curve, hand this to PnlAnalytics
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
        
        if (_calculateCosts) info.realizedProfit += (openTrade->td.profit - (kCSVMapping.commision + kCSVMapping.spread + kCSVMapping.timingCost));
        else info.realizedProfit += openTrade->td.profit;

        info.trades.push_back(openTrade->lockTrade()); openTrade.reset(); inLong=false; inShort=false;
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
        double eq = info.realizedProfit + unreal;
        if (!info.equityCurve.empty() && info.equityCurve.back().first == epochSec) {
            info.equityCurve.back().second = eq;
        } else {
            info.equityCurve.emplace_back(epochSec, eq);
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

    std::vector<TradesInfo::tradeData> returnTradesVector() { return info.trades; }
};
