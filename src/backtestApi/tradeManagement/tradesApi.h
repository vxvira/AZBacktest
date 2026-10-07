#pragma once

#include "tradeData.h"

class Trade {
private:
    double _entryPrice;
    double _tickValue;
    double _tickSize;
    int _entryIdx;
    TradesInfo::TradeDirection _direction;

    bool _lockTrade = false; // Locks trade data in + appends to vec

public:
    /// @brief opens a trade at `entryPrice` in the given direction
    /// @param entryPrice price at which the trade was opened (snapshot; no reference)
    /// @param entryIdx bar index where the trade was opened (for your own bookkeeping)
    /// @param direction Long or Short, flips the sign on P&L
    Trade(double entryPrice, int entryIdx, double tickSize, double tickValue, TradesInfo::TradeDirection direction)
        : _entryPrice(entryPrice)
        , _tickValue(tickValue)
        , _tickSize(tickSize)
        , _entryIdx(entryIdx)
        , _direction(direction)
    {}

    TradesInfo::tradeData td;

    /// @brief update P&L to reflect `currentPrice` (call once per bar while open),
    /// td.profit is in points, multiply by tickValue / tickSize for currency
    TradesInfo::tradeData advanceIdx(double currentPrice) {
        if (!_lockTrade) {
            double diff = currentPrice - _entryPrice;
            td.profit = (_direction == TradesInfo::TradeDirection::Long) ? diff : -diff;
            td.win = td.profit > 0;
        }
        return td;
    }

    /// @brief freezes the trade's P&L and returns it for the caller to record,
    /// once locked, advanceIdx won't update the numbers anymore
    TradesInfo::tradeData lockTrade() { _lockTrade = true; return td; }
};