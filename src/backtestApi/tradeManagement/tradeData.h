#pragma once

#include <vector>
#include <utility>

/// @brief result of a single closed trade, profit, win/loss, and when it closed
/// gets pushed into the global `trades` vec when a Trade is locked
struct tradeData {
    double profit = 0.0; // pts
    bool win = false;
    long long closeEpochSec = 0; // set by TradeApi on close
};

/// @brief every closed trade ends up here, global so the stats functions
/// (returnWinrate, returnCumProfit, etc) can read it without plumbing
std::vector<tradeData> trades;

/// @brief timestamped equity snapshots (realized + open P&L), populated by
/// TradeApi::tick() whenever a timestamp is fed in; consumed by
/// returnProfitOverTime for bucketing into bars
std::vector<std::pair<long long, double>> equityCurve;
double realizedProfit = 0.0;

/// @brief which side a trade is on, flips the sign on P&L math
enum class TradeDirection { Long, Short };
