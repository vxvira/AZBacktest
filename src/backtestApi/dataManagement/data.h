#pragma once

#include <stdexcept>
#include <string>
#include <vector>

/// @brief parallel prices + volumes returned by DataApi::requestDataWindow
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
/// prices is the close, opens/highs/lows complete the bar's OHLC at
/// timeframe>0 (first, highest and lowest price across the bar). in tick mode
/// a tick has no range, so all three just repeat prices[i]
/// every vector here is the same length and indexed the same way, so
/// executedBuys[i] always belongs to prices[i]
struct Bar;
struct BarIterator;

struct DataWindow {
    std::vector<double> prices;        // close
    std::vector<double> opens;
    std::vector<double> highs;
    std::vector<double> lows;
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

    /// [extra] columns from config.toml, extraNames[k] is the name of extras[k] and extras[k][i] belongs
    /// to prices[i] like every other vector here. NaN where a row had no usable value
    /// set by requestDataWindowBy / requestDataWindowNanos. spanDone is false while more rows of the same
    /// span follow in the next call (a chunked return) and true on the span's last window, spanKey is the
    /// key every row of the span mapped to. a plain requestDataWindow leaves them at true / 0
    bool spanDone = true;
    long long spanKey = 0;

    std::vector<std::string> extraNames;
    std::vector<std::vector<double>> extras;

    std::size_t size() const { return prices.size(); }
    bool empty() const { return prices.empty(); }

    // row i as a Bar, and range-for over the rows: for (auto b : w) { b.high ... }
    inline Bar operator[](std::size_t i) const;
    inline BarIterator begin() const;
    inline BarIterator end() const;

    /// @brief the [extra] column called `name`
    /// @throws std::runtime_error if config.toml declares no such column
    const std::vector<double>& extra(const std::string& name) const {
        for (std::size_t k = 0; k < extraNames.size(); ++k)
            if (extraNames[k] == name) return extras[k];
        throw std::runtime_error("DataWindow::extra: no [extra] column called \"" + name + "\" in config.toml");
    }
};

// one row of a DataWindow, the fields are references into its vectors so a Bar is cheap to copy
// close is prices[i], ts is tsRecv[i] (epoch seconds). only valid while the window is
struct Bar {
    const DataWindow& w;
    std::size_t i;
    const double &open, &high, &low, &close, &volume, &buys, &sells, &delta;
    const double &restingBid, &restingAsk, &bid, &ask;
    const long long &ts, &tsEvent, &row;
    const char* action;
    const char* side;

    Bar(const DataWindow& win, std::size_t idx)
        : w(win), i(idx),
          open(win.opens[idx]), high(win.highs[idx]), low(win.lows[idx]), close(win.prices[idx]),
          volume(win.volumes[idx]), buys(win.executedBuys[idx]), sells(win.executedSells[idx]),
          delta(win.deltas[idx]),
          restingBid(win.restingBids[idx]), restingAsk(win.restingAsks[idx]),
          bid(win.bidPrices[idx]), ask(win.askPrices[idx]),
          ts(win.tsRecv[idx]), tsEvent(win.tsEvent[idx]), row(win.rowNumbers[idx]),
          action(win.action[idx]), side(win.side[idx]) {}

    double extra(const std::string& name) const { return w.extra(name)[i]; }
    double extra(std::size_t k) const { return w.extras[k][i]; }
};

struct BarIterator {
    const DataWindow* w;
    std::size_t i;
    Bar operator*() const { return Bar(*w, i); }
    BarIterator& operator++() { ++i; return *this; }
    bool operator!=(const BarIterator& o) const { return i != o.i; }
};

inline Bar DataWindow::operator[](std::size_t i) const { return Bar(*this, i); }
inline BarIterator DataWindow::begin() const { return {this, 0}; }
inline BarIterator DataWindow::end() const { return {this, prices.size()}; }
