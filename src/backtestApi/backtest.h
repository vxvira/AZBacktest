#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backtestApi.h"

// owns the wiring a strategy always needs: the price vector, the data file, DataApi and TradeApi
//
//   Backtest bt(0.25, 0.50, kCSVMapping.path);
//   bt.eachSpan(kDay, 300, 0, [&](const DataWindow& day, const DataWindow& prev) {
//       for (auto b : bt.bars(day)) { /* b.high, bt.trade.openLong(...) */ }
//   });
class Backtest {
public:
    std::vector<double> prices;   // the current bar's price, TradeApi reads prices.back()
    std::unique_ptr<MarketData> md;
    DataApi data;
    TradeApi trade;

    // no file, for when the windows come from somewhere else (eachWindow)
    Backtest(double tickSize, double tickValue)
        : data(prices, tickSize, tickValue), trade(prices, tickSize, tickValue) {}

    Backtest(double tickSize, double tickValue, const std::string& path) : Backtest(tickSize, tickValue) {
        md = std::make_unique<MarketData>(path);
    }

    // 0..1 through the data file
    double progress() const { return md ? md->progress() : 0.0; }

    // calls f(cur, prev) for every window next() hands back until it returns an empty one. prev is
    // empty on the first. closes whatever trade is open at the end and returns the last window
    template <class Next, class F>
    DataWindow eachWindow(Next next, F f) {
        DataWindow prev;
        for (;;) {
            DataWindow cur = next();
            if (cur.empty()) break;
            f(cur, prev);
            prev = std::move(cur);
        }
        trade.closeAll();
        return prev;
    }

    // eachWindow over fixed time spans of the data file, see DataApi::requestDataWindowNanos
    template <class F>
    DataWindow eachSpan(long long spanNanos, int timeframe, long long offsetNanos, F f) {
        if (!md) throw std::runtime_error("Backtest::eachSpan: no data file, give the constructor a path");
        return eachWindow([&] { return data.requestDataWindowNanos(*md, spanNanos, timeframe, 0, offsetNanos); }, f);
    }

    // the bars of a window; stepping to each one sets the current price and ticks the trade
    class Bars {
        Backtest* _bt;
        const DataWindow* _w;
    public:
        struct It {
            Backtest* bt;
            const DataWindow* w;
            std::size_t i;
            Bar operator*() const {
                bt->prices.assign(1, w->prices[i]);
                bt->trade.tick(w->tsRecv[i], w->volumes[i]);
                return Bar(*w, i);
            }
            It& operator++() { ++i; return *this; }
            bool operator!=(const It& o) const { return i != o.i; }
        };
        Bars(Backtest* bt, const DataWindow* w) : _bt(bt), _w(w) {}
        It begin() const { return {_bt, _w, 0}; }
        It end() const { return {_bt, _w, _w->size()}; }
    };

    Bars bars(const DataWindow& w) { return Bars(this, &w); }
};
