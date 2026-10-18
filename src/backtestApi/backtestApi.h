#pragma once

#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <charconv>
#include <optional>
#include <utility>
#include <stdexcept>
#include "tooling.h"
#include "dataManagement/data.h"
#include "tradeManagement/tradesApi.h"
#include "../marketData.h"
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

private:
    // size every parallel vector of `out` for `n` rows, plus one vector per [extra] column
    static void _reserve(DataWindow& out, std::size_t n) {
        out.prices.reserve(n);
        out.opens.reserve(n);
        out.highs.reserve(n);
        out.lows.reserve(n);
        out.volumes.reserve(n);
        out.executedSells.reserve(n);
        out.executedBuys.reserve(n);
        out.deltas.reserve(n);
        out.restingBids.reserve(n);
        out.restingAsks.reserve(n);
        out.bidPrices.reserve(n);
        out.askPrices.reserve(n);
        out.tsRecv.reserve(n);
        out.tsEvent.reserve(n);
        out.tsEventNanos.reserve(n);
        out.rowNumbers.reserve(n);
        out.action.reserve(n);
        out.side.reserve(n);

        // [extra] columns: one parallel vector per name, filled from each tick / bar
        const auto& extraCols = kCSVMapping.extras;
        out.extraNames.reserve(extraCols.size());
        out.extras.resize(extraCols.size());
        for (std::size_t k = 0; k < extraCols.size(); ++k) {
            out.extraNames.push_back(extraCols[k].name);
            out.extras[k].reserve(n);
        }
    }

    // push one tick (isBar = false) or one nextClose bar (isBar = true) onto every vector of `out`
    void _append(DataWindow& out, const Tick& t, bool isBar, void (*whenUnknown)()) {
        double px = 0.0;
        std::from_chars(t.price.data(), t.price.data() + t.price.size(), px);
        out.prices.push_back(px);
        // a tick has no range, open / high / low are just its price
        out.opens.push_back(isBar ? t.open : px);
        out.highs.push_back(isBar ? t.high : px);
        out.lows.push_back(isBar ? t.low : px);
        out.deltas.push_back(t.executedBuys - t.executedSells);
        out.volumes.push_back(t.size);
        out.tsRecv.push_back(mdDetail::tsToEpochSeconds(t.tsRecv));
        out.tsEvent.push_back(t.tsEvent.empty() ? 0 : mdDetail::tsToEpochSeconds(t.tsEvent));
        out.tsEventNanos.push_back(t.tsEvent.empty() ? 0 : mdDetail::tsToEpochNanos(t.tsEvent));
        out.rowNumbers.push_back(t.rowNumber);

        // aggressor split, pushed unconditionally so these stay index-parallel with prices/volumes
        // (a tick with no usable side contributes 0 to both). per bar it is summed across every tick in it
        out.executedBuys.push_back(t.executedBuys);
        out.executedSells.push_back(t.executedSells);
        if (t.unknownVolume > 0.0) whenUnknown(); // custom behavior hook for unclassifiable volume

        // book snapshot at this tick (the closing tick's, for a bar), 0 across the board when the
        // resting columns aren't mapped in config.toml
        out.restingBids.push_back(t.restingBids);
        out.restingAsks.push_back(t.restingAsks);
        out.bidPrices.push_back(t.bidPrice);
        out.askPrices.push_back(t.askPrice);

        out.action.push_back(t.action);
        out.side.push_back(t.side);

        const std::size_t nExtras = kCSVMapping.extras.size();
        for (std::size_t k = 0; k < nExtras; ++k) out.extras[k].push_back(t.extras[k]);

        processedBars++;
    }

    // requestDataWindowBy's place in its current span: open between a chunked return and the next call
    bool _spanOpen = false;
    long long _spanKey = 0;

public:
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
        _reserve(out, period);

        if (timeframe==0) {
            for (int i=0; i<period; i++) {
                std::optional<Tick> tick;
                for (int j=0; j<tickRes; j++) {
                    tick = md.nextTick();
                    if (!tick) break;
                }
                if (!tick) break;
                _append(out, *tick, false, whenUnknown);
            }
        } else {
            for (int i=0; i<period; i++) { // tickRes is tick-by-tick only
                auto bar = md.nextClose(timeframe);
                if (!bar) break;
                _append(out, *bar, true, whenUnknown);
            }
        }
        return out;
    }

    /// @brief the next window of rows that share one `key`, instead of a fixed row count. `key` maps a
    /// row's ts_recv in epoch nanoseconds to any grouping id (a day number, a session, ...); the window
    /// ends where the id changes, so "one day of data" is a call, not bookkeeping in the caller.
    /// rows are ticks (timeframe 0) or timeframe-second bars, a bar belongs to the window of its closing row
    ///
    /// maxRows caps the size of one returned window so a long span never has to sit in memory at once:
    /// the span then comes back in chunks, DataWindow::spanDone is false on every chunk but the last, and
    /// the next call carries on in the same span. 0 = no cap. an empty window means end of data
    ///
    /// @param md      the MarketData source to read from
    /// @param key     callable long long (epoch nanos) -> long long, must not decrease as the data advances
    /// @param timeframe 0 = tick-by-tick, >0 = close every N seconds
    /// @param maxRows rows per returned window, 0 = whole span
    /// @param tickRes timeframe==0 only: keep every Nth tick
    template <typename KeyFn>
    DataWindow requestDataWindowBy(MarketData& md, KeyFn key, int timeframe=0, std::size_t maxRows=0,
                                   void (*whenUnknown)()=[](){}, int tickRes=1) {
        DataWindow out;
        _reserve(out, maxRows ? std::min<std::size_t>(maxRows, 4096) : 1024);

        // one record, a tick or a bar, nullopt at the end of the data
        auto read = [&]() -> std::optional<Tick> {
            if (timeframe != 0) return md.nextClose(timeframe);
            std::optional<Tick> t;
            for (int j = 0; j < tickRes; ++j) { t = md.nextTick(); if (!t) break; }
            return t;
        };

        for (;;) {
            // chunk is full: look one record ahead (and put it back) to say whether the span goes on
            if (maxRows && out.prices.size() >= maxRows) {
                const std::size_t before = md.byteOffset();
                auto next = read();
                out.spanKey = _spanKey;
                if (!next || key(mdDetail::tsToEpochNanos(next->tsRecv)) != _spanKey) {
                    out.spanDone = true;
                    _spanOpen = false;
                } else {
                    out.spanDone = false;
                }
                if (next) md.seekTo(before);
                return out;
            }

            const std::size_t before = md.byteOffset();
            auto rec = read();
            if (!rec) { // end of data closes whatever span was open
                out.spanKey = _spanKey;
                out.spanDone = true;
                _spanOpen = false;
                return out;
            }

            const long long k = key(mdDetail::tsToEpochNanos(rec->tsRecv));
            if (!_spanOpen) {
                _spanKey = k;
                _spanOpen = true;
            } else if (k != _spanKey) { // first record of the next span, hand it back to the reader
                md.seekTo(before);
                out.spanKey = _spanKey;
                out.spanDone = true;
                _spanOpen = false;
                return out;
            }
            _append(out, *rec, timeframe != 0, whenUnknown);
        }
    }

    /// @brief requestDataWindowBy with fixed length windows: [k * spanNanos + offsetNanos, (k + 1) * spanNanos
    /// + offsetNanos) for whole k, so spans are anchored to the clock, not to the first row. a gap in the data
    /// just skips the empty spans. offsetNanos shifts the grid, e.g. -5h turns UTC days into fixed-offset
    /// New York days (it doesn't follow daylight saving, use requestDataWindowBy with your own key for that)
    /// @param spanNanos  window length in nanoseconds, must be > 0
    /// @param offsetNanos where the grid starts relative to the epoch
    DataWindow requestDataWindowNanos(MarketData& md, long long spanNanos, int timeframe=0, std::size_t maxRows=0,
                                      long long offsetNanos=0, void (*whenUnknown)()=[](){}, int tickRes=1) {
        if (spanNanos <= 0) throw std::invalid_argument("requestDataWindowNanos: spanNanos must be > 0");
        return requestDataWindowBy(md, [spanNanos, offsetNanos](long long ns) {
            const long long x = ns - offsetNanos;
            return x >= 0 ? x / spanNanos : -((-x + spanNanos - 1) / spanNanos); // floor
        }, timeframe, maxRows, whenUnknown, tickRes);
    }

    // other helper funcs and such

    void overrideProcessedBars(int newValue) { processedBars = newValue; }

    /// @brief raw value at [row, col] straight from the CSV, an escape hatch for
    /// any column dataConfig.h doesn't map to a named field (nextTick/nextClose
    /// only ever parse timestamp/price/size/aggressor). row is 0-indexed and
    /// counts data rows after the header;
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

    struct Order { int dir; int idx; double price; double queue; double seen; bool limit; std::chrono::steady_clock::time_point at; };
    std::vector<Order> _orders;

    void fillOrders(double vol) {
        const auto now = std::chrono::steady_clock::now();
        const double px = _prices.back();
        _orders.erase(std::remove_if(_orders.begin(), _orders.end(), [&](Order& o) {
            if (std::chrono::duration<double, std::milli>(now - o.at).count() < kCSVMapping.timingCost) return false;
            double fillPx = px;
            if (o.limit) {
                const double through = (o.price - px) * o.dir;
                if (through < 0) return false;
                if (through == 0 && (o.seen += vol) <= o.queue) return false;
                fillPx = o.price;
            }
            openTrades.emplace_back(fillPx, o.idx, _tickSize, _tickValue,
                o.dir == 1 ? TradesInfo::TradeDirection::Long : TradesInfo::TradeDirection::Short, o.limit);
            (o.dir == 1 ? inLong : inShort) = true;
            return true;
        }), _orders.end());
    }

    int addOrder(int dir, int idx, bool limit, double price = 0, double queue = 0) {
        if ((!canOverlap && !_orders.empty()) || !legalEntry(dir)) return 0;
        _orders.push_back({dir, idx, price, queue, 0, limit, std::chrono::steady_clock::now()});
        fillOrders(0);
        return 1;
    }


public:
    /// @brief bind `prices` (by reference, caller must keep it alive) and the
    ///        per-instrument tick metadata that every new Trade needs
    /// @param prices    live price window; TradeApi reads `.back()` at each entry
    /// @param tickSize  instrument tick size, forwarded to Trade
    /// @param tickValue instrument tick value, forwarded to Trade
    TradeApi(std::vector<double>& prices, double tickSize, double tickValue)
        : _prices(prices), _tickSize(tickSize), _tickValue(tickValue) {}

    // Trade management
    std::vector<Trade> openTrades;
    TradesInfo info; // closed trades + equity curve, hand this to PnlAnalytics
    bool inLong = false;
    bool inShort = false;

    bool canOverlap = false; // can multiple trades be held at once

    long long lastEpochSec = 0; // most recent timestamp fed to tick(); stamped onto trades at close

    bool legalEntry(int dir) {
        if (dir == 1) {
            if (inShort) return false;
            if (!inLong) return true;
            if (inLong && canOverlap) return true;
            return false;
        }
        if (dir == -1) {
            if (inLong) return false;
            if (!inShort) return true;
            if (inShort && canOverlap) return true; 
            return false;
        }
        std::cerr << "Illegal direction passed to `legalEntry(int dir). Takes -1 for short, 1 for long" << std::endl;
        return false;
    }

    int openLong(int idx) { return addOrder(1, idx, false); }
    int openShort(int idx) { return addOrder(-1, idx, false); }

    /// @brief limit order, fills at `price` once price trades through it, or once more than `queue` volume trades at it
    int placeLongLimit(int idx, double price, double queue = 0) { return addOrder(1, idx, true, price, queue); }
    int placeShortLimit(int idx, double price, double queue = 0) { return addOrder(-1, idx, true, price, queue); }

    void closeTrade(std::size_t i = 0) {
        if (i >= openTrades.size()) return;
        Trade& t = openTrades[i];
        t.td.closeEpochSec = lastEpochSec;
        info.realizedProfit += t.td.profit - kCSVMapping.commision - (t.isLimit ? kCSVMapping.spread : 0);
        info.trades.push_back(t.lockTrade());
        openTrades.erase(openTrades.begin() + i);
        inLong = inShort = false;
        for (auto& o : openTrades) (o.direction() == TradesInfo::TradeDirection::Long ? inLong : inShort) = true;
    }

    void closeAll() {
        while (!openTrades.empty()) closeTrade();
    }

    std::pair<bool,bool> tick(long long epochSec, double vol = 0.0) {
        fillOrders(vol);
        double unreal = 0.0;
        for (auto& t : openTrades) { t.advanceIdx(_prices.back()); unreal += t.td.profit; }
        lastEpochSec = epochSec;
        double eq = info.realizedProfit + unreal;
        if (!info.equityCurve.empty() && info.equityCurve.back().first == epochSec) {
            info.equityCurve.back().second = eq;
        } else {
            info.equityCurve.emplace_back(epochSec, eq);
        }
        return {inLong, inShort};
    }

    std::pair<bool,bool> tick(std::string_view timestamp = {}, double vol = 0.0) {
        if (timestamp.empty()) {
            fillOrders(vol);
            for (auto& t : openTrades) t.advanceIdx(_prices.back());
            return {inLong, inShort};
        }
        return tick(mdDetail::tsToEpochSeconds(timestamp), vol);
    }

    std::vector<TradesInfo::tradeData> returnTradesVector() { return info.trades; }
};
