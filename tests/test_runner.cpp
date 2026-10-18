// Tests for the Bar row proxy, Backtest (the runner), MarketData::progress and the day indicators.
// Rows use epoch-second timestamps so each test says exactly where its rows fall.

#include <cmath>
#include <set>

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"
#include "src/backtestApi/backtestApi.h"
#include "src/backtestApi/backtest.h"
#include "src/analytics/DayIndicators.h"

using azt::TempCsv;
using azt::useFixtureMapping;

namespace {

constexpr long long kSec  = 1'000'000'000LL;
constexpr long long kBase = 1'700'000'040LL; // on a minute, not on a day boundary

void epochMapping() {
    useFixtureMapping();
    kCSVMapping.tsRecvCol = 0;
    kCSVMapping.priceCol  = 1;
    kCSVMapping.sizeCol   = 2;
    kCSVMapping.aggressor = -1;
    kCSVMapping.restingBidCol = kCSVMapping.restingAskCol = -1;
    kCSVMapping.bidPriceCol = kCSVMapping.askPriceCol = -1;
    kCSVMapping.actionCol = -1;
    kCSVMapping.timeFormat = TimeFormat::EpochSeconds;
}

// ts offset (s from kBase), price, size
struct R { int t; double px; double sz; };

std::string rows(std::initializer_list<R> rs) {
    std::string s = "t,px,sz\n";
    for (auto& r : rs) s += std::to_string(kBase + r.t) + "," + std::to_string(r.px) + "," + std::to_string(r.sz) + "\n";
    return s;
}

DataWindow ticks(const TempCsv& csv, int n = 100) {
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);
    return data.requestDataWindow(md, n);
}

} // namespace

// ---------------------------------------------------------------- Bar

TEST(barReadsTheSameRowAsTheVectors) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 3}, {5, 101, 4}}));
    DataWindow w = ticks(csv);
    REQUIRE(w.size() == 2);

    Bar b = w[1];
    CHECK_EQ((int)b.i, 1);
    CHECK_F(b.close, 101.f);
    CHECK_F(b.high, 101.f);
    CHECK_F(b.volume, 4.f);
    CHECK_EQ(b.ts, kBase + 5);
    CHECK(&b.close == &w.prices[1]); // a reference, not a copy
}

TEST(rangeForWalksEveryBarInOrder) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}, {1, 101, 1}, {2, 102, 1}}));
    DataWindow w = ticks(csv);

    std::vector<double> got;
    for (auto b : w) got.push_back(b.close);
    CHECK(got == (std::vector<double>{100, 101, 102}));
    CHECK(!w.empty());
}

TEST(barReachesExtrasByNameAndIndex) {
    epochMapping();
    kCSVMapping.extras = {cfgDetail::parseExtra("iv", "iv")};
    TempCsv csv("t,px,sz,iv\n" + std::to_string(kBase) + ",100,1,0.25\n");
    DataWindow w = ticks(csv);

    CHECK_NEAR(w[0].extra("iv"), 0.25, 1e-9);
    CHECK_NEAR(w[0].extra(0), 0.25, 1e-9);
}

// ---------------------------------------------------------------- progress

TEST(progressGoesFromZeroToOne) {
    epochMapping();
    std::string r = "t,px,sz\n";
    for (int i = 0; i < 200; ++i) r += std::to_string(kBase + i) + ",100,1\n";
    TempCsv csv(r);
    MarketData md(csv.path());

    CHECK_NEAR(md.progress(), 0.0, 0.05);
    for (int i = 0; i < 100; ++i) md.nextTick();
    const double half = md.progress();
    CHECK(half > 0.3 && half < 0.7);
    while (md.nextTick()) {}
    CHECK_NEAR(md.progress(), 1.0, 1e-9);
}

// ---------------------------------------------------------------- Backtest

TEST(eachSpanHandsOverTheDayAndTheOneBefore) {
    epochMapping();
    const long long day = 86400;
    const long long start = (kBase / day + 1) * day; // a day boundary after kBase
    std::string r = "t,px,sz\n";
    for (int d = 0; d < 3; ++d)
        for (int i = 0; i < 4; ++i) r += std::to_string(start + d * day + i * 60) + "," + std::to_string(100 + d * 10 + i) + ",1\n";
    TempCsv csv(r);

    Backtest bt(0.25, 0.5, csv.path());
    std::vector<std::pair<std::size_t, std::size_t>> seen; // (day size, prev size)
    std::vector<double> firstOfPrev;
    DataWindow last = bt.eachSpan(day * kSec, 0, 0, [&](const DataWindow& cur, const DataWindow& prev) {
        seen.push_back({cur.size(), prev.size()});
        firstOfPrev.push_back(prev.empty() ? -1 : prev.prices.front());
    });

    CHECK_EQ((int)seen.size(), 3);
    CHECK((seen[0] == std::pair<std::size_t, std::size_t>(4, 0))); // first day has no previous
    CHECK((seen[1] == std::pair<std::size_t, std::size_t>(4, 4)));
    CHECK_F(firstOfPrev[1], 100.f);
    CHECK_F(firstOfPrev[2], 110.f);
    CHECK_F(last.prices.front(), 120.f); // the last window comes back
}

TEST(barsSetThePriceAndTickTheTrade) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}, {60, 104, 1}, {120, 110, 1}}));
    DataWindow w = ticks(csv);

    Backtest bt(0.25, 0.5);
    std::vector<double> seenPrice;
    int i = 0;
    for (auto b : bt.bars(w)) {
        seenPrice.push_back(bt.prices.back());
        CHECK_F(bt.prices.back(), b.close);
        if (i == 0) bt.trade.openLong(i);
        i++;
    }
    CHECK(seenPrice == (std::vector<double>{100, 104, 110}));
    REQUIRE(!bt.trade.openTrades.empty());
    CHECK_F(bt.trade.openTrades[0].td.profit, 10.f); // marked to the last bar
    CHECK_EQ(bt.trade.lastEpochSec, kBase + 120);
}

TEST(eachWindowClosesTheOpenTradeAtTheEnd) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}, {60, 105, 1}}));
    DataWindow w = ticks(csv);

    Backtest bt(0.25, 0.5);
    int calls = 0;
    bt.eachWindow([&] { return calls++ == 0 ? w : DataWindow(); },
                  [&](const DataWindow& cur, const DataWindow&) {
        for (auto b : bt.bars(cur)) if (b.i == 0) bt.trade.openLong(0);
    });
    CHECK(bt.trade.openTrades.empty());
    CHECK_EQ((int)bt.trade.info.trades.size(), 1);
}

TEST(eachSpanWithoutAFileThrows) {
    Backtest bt(0.25, 0.5);
    bool threw = false;
    try { bt.eachSpan(kSec, 0, 0, [](const DataWindow&, const DataWindow&) {}); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ---------------------------------------------------------------- day indicators

// bars: (h, l, c, vol) closing 10, 12, 11 with a gap up on the last
//   tr: 4 (no prev), max(3, |14-10|, |11-10|)=4, max(5, |17-12|, |12-12|)=5  -> atr 13/3
TEST(dayStatsOverHandBuiltBars) {
    epochMapping();
    kCSVMapping.rowsAreBars = true;
    kCSVMapping.openCol = 3; kCSVMapping.highCol = 4; kCSVMapping.lowCol = 5;
    TempCsv csv("t,c,v,o,h,l\n"
                + std::to_string(kBase)      + ",10,100,8,12,8\n"
                + std::to_string(kBase + 60) + ",12,200,10,14,11\n"
                + std::to_string(kBase + 120) + ",12,300,12,17,12\n");
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);
    DataWindow w = data.requestDataWindow(md, 10, 60);
    REQUIRE(w.size() == 3);

    DayStats s = dayStats(w);
    CHECK_EQ((int)s.bars, 3);
    CHECK_F(s.open, 8.f);
    CHECK_F(s.high, 17.f);
    CHECK_F(s.low, 8.f);
    CHECK_F(s.close, 12.f);
    CHECK_F(s.range, 9.f);
    CHECK_F(s.volume, 600.f);
    CHECK_NEAR(s.atr, 13.0 / 3.0, 1e-9);
    // vwap: typical prices 10, 12.333.., 13.666.. weighted 100, 200, 300
    CHECK_NEAR(s.vwap, (10.0 * 100 + (14 + 11 + 12) / 3.0 * 200 + (17 + 12 + 12) / 3.0 * 300) / 600.0, 1e-9);
    CHECK_NEAR(s.realizedVol, std::sqrt(std::pow(std::log(12.0 / 10.0), 2) + 0.0), 1e-9);

    CHECK_NEAR(atr(w), s.atr, 1e-12);
    CHECK_NEAR(vwap(w), s.vwap, 1e-12);
}

TEST(keepLeavesBarsOutOfEveryNumber) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}, {60, 110, 1}, {120, 90, 1}}));
    DataWindow w = ticks(csv);

    auto early = [](const Bar& b) { return b.ts < kBase + 100; };
    DayStats s = dayStats(w, early);
    CHECK_EQ((int)s.bars, 2);
    CHECK_F(s.high, 110.f);
    CHECK_F(s.low, 100.f);
    CHECK_F(s.close, 110.f);
}

TEST(prevCloseAddsTheOvernightGap) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}}));
    DataWindow w = ticks(csv);

    CHECK_NEAR(atr(w, [](const Bar&) { return true; }), 0.0, 1e-12);          // a tick has no range, no gap known
    CHECK_NEAR(atr(w, [](const Bar&) { return true; }, 95.0), 5.0, 1e-12);    // opened 5 above yesterday's close
}

TEST(trueRangeIsPerBar) {
    epochMapping();
    TempCsv csv(rows({{0, 100, 1}, {1, 103, 1}, {2, 101, 1}}));
    DataWindow w = ticks(csv);

    auto tr = trueRange(w, 99.0);
    REQUIRE(tr.size() == 3);
    CHECK_NEAR(tr[0], 1.0, 1e-9); // gap from 99
    CHECK_NEAR(tr[1], 3.0, 1e-9);
    CHECK_NEAR(tr[2], 2.0, 1e-9);
}

TEST(emptyWindowGivesZeros) {
    DataWindow w;
    DayStats s = dayStats(w);
    CHECK_EQ((int)s.bars, 0);
    CHECK_NEAR(s.atr, 0.0, 1e-12);
    CHECK_NEAR(atr(w), 0.0, 1e-12);
}
