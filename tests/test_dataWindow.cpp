// Unit tests for Handling::requestDataWindow, the layer that turns Ticks into
// the parallel vectors a strategy actually reads.

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/backtestApi.h"

using azt::TempCsv;
using azt::useFixtureMapping;

// every vector in a DataWindow is indexed the same way, so a strategy can pair
// executedBuys[i] with prices[i]. if one of them ever stops being pushed
// unconditionally this is the test that catches it
static void checkParallelLengths(const DataWindow& w) {
    std::size_t n = w.prices.size();
    CHECK_EQ(w.opens.size(),         n);
    CHECK_EQ(w.highs.size(),         n);
    CHECK_EQ(w.lows.size(),          n);
    CHECK_EQ(w.volumes.size(),       n);
    CHECK_EQ(w.executedBuys.size(),  n);
    CHECK_EQ(w.executedSells.size(), n);
    CHECK_EQ(w.deltas.size(),        n);
    CHECK_EQ(w.restingBids.size(),   n);
    CHECK_EQ(w.restingAsks.size(),   n);
    CHECK_EQ(w.bidPrices.size(),     n);
    CHECK_EQ(w.askPrices.size(),     n);
    CHECK_EQ(w.tsRecv.size(),       n);
    CHECK_EQ(w.tsEvent.size(),      n);
    CHECK_EQ(w.tsEventNanos.size(), n);
    CHECK_EQ(w.rowNumbers.size(),   n);
}

// ---------------------------------------------------------------- tick mode

TEST(tickModeReturnsEveryRow) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    CHECK_EQ(w.prices.size(), (std::size_t)6);
    checkParallelLengths(w);
}

TEST(tickModeParsesPricesAndVolumes) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.prices.size() == 6);

    const double wantPx[6]  = {5000.25, 5000.50, 5000.75, 5001.00, 5000.50, 5000.25};
    const double wantVol[6] = {3, 7, 2, 5, 4, 6};
    for (int i = 0; i < 6; i++) {
        CHECK_F(w.prices[i],  wantPx[i]);
        CHECK_F(w.volumes[i], wantVol[i]);
    }
}

TEST(tickModeCarriesRestingBookThrough) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.restingBids.size() == 6);

    const double wantBid[6] = {40, 41, 45, 60, 61, 30};
    const double wantAsk[6] = {55, 52, 50, 20, 19, 33};
    for (int i = 0; i < 6; i++) {
        CHECK_F(w.restingBids[i], wantBid[i]);
        CHECK_F(w.restingAsks[i], wantAsk[i]);
    }
}

TEST(tickModeCarriesBidAskPricesThrough) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.bidPrices.size() == 6);
    REQUIRE(w.askPrices.size() == 6);

    const double wantBid[6] = {5000.00, 5000.25, 5000.50, 5000.75, 5000.25, 5000.00};
    const double wantAsk[6] = {5000.50, 5000.75, 5001.00, 5001.25, 5000.75, 5000.50};
    for (int i = 0; i < 6; i++) {
        CHECK_F(w.bidPrices[i], wantBid[i]);
        CHECK_F(w.askPrices[i], wantAsk[i]);
    }
}

// same guarantee as the resting vectors: always pushed, all zero when unmapped
TEST(tickModeBidAskVectorsStayParallelWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.bidPriceCol = -1;
    kCSVMapping.askPriceCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    checkParallelLengths(w);
    for (std::size_t i = 0; i < w.bidPrices.size(); i++) {
        CHECK_F(w.bidPrices[i], 0.0);
        CHECK_F(w.askPrices[i], 0.0);
    }
}

// the resting vectors are pushed unconditionally so the index-parallel
// guarantee holds even when nothing maps them, they just come back all zero
TEST(tickModeRestingVectorsStayParallelWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.restingBidCol = -1;
    kCSVMapping.restingAskCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    checkParallelLengths(w);
    CHECK_EQ(w.restingBids.size(), (std::size_t)6);
    for (std::size_t i = 0; i < w.restingBids.size(); i++) {
        CHECK_F(w.restingBids[i], 0.0);
        CHECK_F(w.restingAsks[i], 0.0);
    }
}

TEST(tickModeSplitsVolumeByAggressor) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.prices.size() == 6);

    const double wantBuys[6]  = {3, 0, 0, 5, 0, 6};
    const double wantSells[6] = {0, 7, 0, 0, 4, 0};
    for (int i = 0; i < 6; i++) {
        CHECK_F(w.executedBuys[i],  wantBuys[i]);
        CHECK_F(w.executedSells[i], wantSells[i]);
        // row 2 has an unclassifiable side, so its volume is in neither
        CHECK(w.executedBuys[i] + w.executedSells[i] <= w.volumes[i] + 1e-4f);
    }
}

// deltas[i] is executedBuys[i] - executedSells[i] (orderflow delta)
TEST(deltasAreOrderflowDelta) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.deltas.size() == 6);

    CHECK_F(w.deltas[0],  3.0);    // B size=3: 3-0
    CHECK_F(w.deltas[1], -7.0);    // A size=7: 0-7
    CHECK_F(w.deltas[2],  0.0);    // X size=2: 0-0 (unknown)
    CHECK_F(w.deltas[3],  5.0);    // B size=5: 5-0
    CHECK_F(w.deltas[4], -4.0);    // A size=4: 0-4
    CHECK_F(w.deltas[5],  6.0);    // B size=6: 6-0
}

// orderflow delta is self-contained per tick, so batching doesn't matter
// � each tick's delta is just its own executedBuys - executedSells
TEST(deltasAreConsistentAcrossBatches) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);

    auto first = h.requestDataWindow(md, 3);
    REQUIRE(first.prices.size() == 3);
    prices = first.prices; // what a strategy does between batches

    auto second = h.requestDataWindow(md, 3);
    REQUIRE(second.prices.size() == 3);
    // second batch row 0 is B size=5: delta = 5
    CHECK_F(second.deltas[0], 5.0);
}

TEST(periodLongerThanFileStopsAtEof) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 500);

    CHECK_EQ(w.prices.size(), (std::size_t)6);
    checkParallelLengths(w);
}

TEST(tsRecvIsParallelToPrices) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    CHECK_EQ(w.tsRecv.size(), w.prices.size());
    long long base = mdDetail::civilToDays(2025, 6, 1) * 86400LL + 22 * 3600LL;
    CHECK_EQ(w.tsRecv[0], base);
    CHECK_EQ(w.tsRecv[1], base + 10);
}

// tsEvent/rowNumbers stay index-parallel and default to 0 when tsEventCol/
// rowNumberCol aren't mapped, same "always pushed" guarantee as the resting
// vectors
TEST(tsEventAndRowNumberStayParallelWhenUnmapped) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    checkParallelLengths(w);
    for (std::size_t i = 0; i < w.tsEvent.size(); i++) {
        CHECK_EQ(w.tsEvent[i], 0LL);
        CHECK_EQ(w.tsEventNanos[i], 0LL);
        CHECK_EQ(w.rowNumbers[i], 0LL);
    }
}

// tickRes reads and discards ticks between the ones it keeps, so the discarded
// rows' volume is dropped outright rather than folded into the kept tick
TEST(tickResKeepsEveryNthTick) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 3, 0, [](){}, 2);

    REQUIRE(w.prices.size() == 3);
    // rows 1, 3 and 5 survive, rows 0/2/4 get read and thrown away
    CHECK_F(w.prices[0], 5000.50);
    CHECK_F(w.prices[1], 5001.00);
    CHECK_F(w.prices[2], 5000.25);
    CHECK_F(w.restingBids[0], 41.0);
    CHECK_F(w.restingBids[1], 60.0);
    CHECK_F(w.restingBids[2], 30.0);
    checkParallelLengths(w);
}

// whenUnknown fires once per bar that carried unclassifiable volume. the
// fixture has exactly one such row
static int unknownHits = 0;

TEST(whenUnknownFiresForUnclassifiableVolume) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    unknownHits = 0;
    h.requestDataWindow(md, 10, 0, [](){ unknownHits++; });

    CHECK_EQ(unknownHits, 1);
}

TEST(whenUnknownNeverFiresWhenEverySideClassifies) {
    useFixtureMapping();
    TempCsv csv(std::string(azt::kFixtureHeader) +
        "2025-06-01T22:00:00.000000000Z,ESM5,5000.25,3,B,40,55\n"
        "2025-06-01T22:00:10.000000000Z,ESM5,5000.50,7,A,41,52\n");
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    unknownHits = 0;
    h.requestDataWindow(md, 10, 0, [](){ unknownHits++; });

    CHECK_EQ(unknownHits, 0);
}

// mapping tsEventCol/rowNumberCol pulls real values through, mirroring how
// restingBidCol/bidPriceCol etc are exercised above
TEST(tsEventAndRowNumberAreParsedWhenMapped) {
    useFixtureMapping();
    kCSVMapping.tsEventCol   = 0; // same column as tsRecvCol, just to prove the plumbing
    kCSVMapping.rowNumberCol = 3; // reuse the size column, values are known ints
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);
    REQUIRE(w.tsEvent.size() == 6);

    const long long wantRow[6] = {3, 7, 2, 5, 4, 6};
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(w.tsEvent[i], w.tsRecv[i]);
        CHECK_EQ(w.tsEventNanos[i], w.tsRecv[i] * 1'000'000'000LL);
        CHECK_EQ(w.rowNumbers[i], wantRow[i]);
    }
}

// tsEventNanos keeps the 9 fractional digits that tsEvent (whole seconds) drops
TEST(tsToEpochNanosKeepsFractionalDigits) {
    useFixtureMapping();
    long long whole = mdDetail::tsToEpochSeconds("2026-06-21T12:00:06.108130665Z");
    CHECK_EQ(mdDetail::tsToEpochNanos("2026-06-21T12:00:06.108130665Z"), whole * 1'000'000'000LL + 108130665LL);
    CHECK_EQ(mdDetail::tsToEpochNanos("2026-06-21T12:00:06.000000000Z"), whole * 1'000'000'000LL);
}

// ---------------------------------------------------------------- bar mode

TEST(barModeAggregatesVolumePerBar) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.prices.size() == 2);
    checkParallelLengths(w);
    CHECK_F(w.volumes[0], 17.0);
    CHECK_F(w.volumes[1], 10.0);
    CHECK_F(w.prices[0], 5001.00);
    CHECK_F(w.prices[1], 5000.25);
}

TEST(barModeSumsAggressorSplit) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.prices.size() == 2);
    CHECK_F(w.executedBuys[0],  8.0);
    CHECK_F(w.executedSells[0], 7.0);
    CHECK_F(w.executedBuys[1],  6.0);
    CHECK_F(w.executedSells[1], 4.0);
}

// unlike volume, resting size is a snapshot, so a bar reports the closing
// tick's book. summing bar 1 would give 186/177 instead
TEST(barModeRestingSizesAreTheClosingSnapshot) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.prices.size() == 2);
    CHECK_F(w.restingBids[0], 60.0);
    CHECK_F(w.restingAsks[0], 20.0);
    CHECK_F(w.restingBids[1], 30.0);
    CHECK_F(w.restingAsks[1], 33.0);
}

// the quote is a snapshot too, bar 1 reports its closing row's 5000.75/5001.25
TEST(barModeBidAskPricesAreTheClosingSnapshot) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.prices.size() == 2);
    CHECK_F(w.bidPrices[0], 5000.75);
    CHECK_F(w.askPrices[0], 5001.25);
    CHECK_F(w.bidPrices[1], 5000.00);
    CHECK_F(w.askPrices[1], 5000.50);
    checkParallelLengths(w);
}

TEST(barModeDeltasAreOrderflowDelta) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.deltas.size() == 2);
    CHECK_F(w.deltas[0],  1.0);      // bar1: buys=8, sells=7 -> 1
    CHECK_F(w.deltas[1],  2.0);      // bar2: buys=6, sells=4 -> 2
}

// bar1 trades 5000.25 -> 5000.50 -> 5000.75 -> 5001.00, bar2 5000.50 -> 5000.25
TEST(barModeReportsOHLC) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10, 60);

    REQUIRE(w.prices.size() == 2);
    checkParallelLengths(w);
    CHECK_F(w.opens[0],  5000.25);
    CHECK_F(w.highs[0],  5001.00);
    CHECK_F(w.lows[0],   5000.25);
    CHECK_F(w.prices[0], 5001.00);
    CHECK_F(w.opens[1],  5000.50);
    CHECK_F(w.highs[1],  5000.50);
    CHECK_F(w.lows[1],   5000.25);
    CHECK_F(w.prices[1], 5000.25);
}

// a tick has no range, so open/high/low all repeat its price
TEST(tickModeOHLCRepeatsPrice) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    std::vector<double> prices;
    Handling h(prices, 0.25, 12.5, false);
    auto w = h.requestDataWindow(md, 10);

    REQUIRE(w.prices.size() == 6);
    for (int i = 0; i < 6; i++) {
        CHECK_F(w.opens[i], w.prices[i]);
        CHECK_F(w.highs[i], w.prices[i]);
        CHECK_F(w.lows[i],  w.prices[i]);
    }
}
