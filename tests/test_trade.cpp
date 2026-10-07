// Unit tests for Trade/TradeApi P&L, which is tracked in points

#include "tests/testFramework.h"

#include "src/backtestApi/backtestApi.h"

TEST(longProfitIsInPoints) {
    Trade t(5000.0, 0, 0.25, 0.50, TradeDirection::Long);
    CHECK_F(t.advanceIdx(5010.0).profit, 10.0);
    CHECK_F(t.advanceIdx(4995.0).profit, -5.0);
}

TEST(shortProfitIsInPoints) {
    Trade t(5000.0, 0, 0.25, 0.50, TradeDirection::Short);
    CHECK_F(t.advanceIdx(4990.0).profit, 10.0);
    CHECK(t.td.win);
}

TEST(closedTradeKeepsPoints) {
    std::vector<double> prices{5000.0};
    TradeApi h(prices, 0.25, 0.50, false);
    h.openLong(0);
    prices.back() = 5012.5;
    h.tick(1);
    h.closeTrade();
    REQUIRE(trades.size() == 1);
    CHECK_F(trades.back().profit, 12.5);
    CHECK_F(returnCumProfit(), 12.5);
}
