#pragma once

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
};
