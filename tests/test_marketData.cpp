// Unit tests for the CSV reader and the mdDetail parsing helpers behind it.

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"

using azt::TempCsv;
using azt::useFixtureMapping;

// ---------------------------------------------------------------- field/nFields

TEST(fieldExtractsColumnsByIndex) {
    const char* row = "a,bb,ccc,dddd";
    const char* end = row + std::strlen(row);
    CHECK_EQ(mdDetail::field(row, end, 0), "a");
    CHECK_EQ(mdDetail::field(row, end, 1), "bb");
    CHECK_EQ(mdDetail::field(row, end, 2), "ccc");
    CHECK_EQ(mdDetail::field(row, end, 3), "dddd"); // last field, no trailing comma
}

TEST(fieldReturnsEmptyPastEndOfRow) {
    const char* row = "a,b";
    const char* end = row + std::strlen(row);
    CHECK_EQ(mdDetail::field(row, end, 5), "");
}

TEST(nFieldsExtractsEveryRequestedColumn) {
    const char* row = "a,bb,ccc,dddd,e";
    const char* end = row + std::strlen(row);
    const int cols[3] = {0, 2, 4};
    std::string_view f[3];
    mdDetail::nFields(row, end, cols, 3, f);
    CHECK_EQ(f[0], "a");
    CHECK_EQ(f[1], "ccc");
    CHECK_EQ(f[2], "e");
}

// the two/three/fourFields helpers bail out on their LAST index, so they only
// work when the config happens to list columns in ascending order. nFields
// stops at whichever index is largest instead, which is what lets nextTick
// pass an arbitrary column mapping straight through
TEST(nFieldsHandlesUnsortedColumnIndices) {
    const char* row = "a,bb,ccc,dddd,e";
    const char* end = row + std::strlen(row);
    const int cols[4] = {3, 0, 4, 1};
    std::string_view f[4];
    mdDetail::nFields(row, end, cols, 4, f);
    CHECK_EQ(f[0], "dddd");
    CHECK_EQ(f[1], "a");
    CHECK_EQ(f[2], "e");
    CHECK_EQ(f[3], "bb");
}

// -1 means the column is not configured, it has to come back empty rather than
// matching column 0 or walking off the row
TEST(nFieldsLeavesNegativeColumnsEmpty) {
    const char* row = "a,bb,ccc";
    const char* end = row + std::strlen(row);
    const int cols[3] = {-1, 1, -1};
    std::string_view f[3];
    mdDetail::nFields(row, end, cols, 3, f);
    CHECK_EQ(f[0], "");
    CHECK_EQ(f[1], "bb");
    CHECK_EQ(f[2], "");
}

TEST(nFieldsAllNegativeTouchesNothing) {
    const char* row = "a,bb,ccc";
    const char* end = row + std::strlen(row);
    const int cols[2] = {-1, -1};
    std::string_view f[2] = {"sentinel", "sentinel"};
    mdDetail::nFields(row, end, cols, 2, f);
    CHECK_EQ(f[0], "sentinel");
    CHECK_EQ(f[1], "sentinel");
}

TEST(nFieldsRepeatedIndexFillsEverySlot) {
    const char* row = "a,bb,ccc";
    const char* end = row + std::strlen(row);
    const int cols[2] = {1, 1};
    std::string_view f[2];
    mdDetail::nFields(row, end, cols, 2, f);
    CHECK_EQ(f[0], "bb");
    CHECK_EQ(f[1], "bb");
}

TEST(nFieldsShortRowLeavesMissingColumnsEmpty) {
    const char* row = "a,bb";
    const char* end = row + std::strlen(row);
    const int cols[2] = {0, 6};
    std::string_view f[2];
    mdDetail::nFields(row, end, cols, 2, f);
    CHECK_EQ(f[0], "a");
    CHECK_EQ(f[1], "");
}

TEST(parseOptionalFloatLeavesDestinationAloneWhenEmpty) {
    double v = 7.0;
    mdDetail::parseOptionalFloat("", v);
    CHECK_F(v, 7.0);
    mdDetail::parseOptionalFloat("42", v);
    CHECK_F(v, 42.0);
}

// ---------------------------------------------------------------- date helpers

TEST(civilDaysRoundTrip) {
    struct { int y; unsigned m, d; } dates[] = {
        {1970, 1, 1}, {2000, 2, 29}, {2024, 2, 29}, {2025, 6, 1}, {2099, 12, 31}, {1969, 12, 31},
    };
    for (auto& x : dates) {
        auto c = mdDetail::daysToCivil(mdDetail::civilToDays(x.y, x.m, x.d));
        CHECK_EQ(c.y, x.y);
        CHECK_EQ((int)c.m, (int)x.m);
        CHECK_EQ((int)c.d, (int)x.d);
    }
}

TEST(epochAtUnixZero) {
    CHECK_EQ(mdDetail::civilToDays(1970, 1, 1), 0LL);
}

TEST(tsToEpochSecondsParsesIso8601) {
    useFixtureMapping();
    // 2025-06-01T22:00:00Z
    long long expected = mdDetail::civilToDays(2025, 6, 1) * 86400LL + 22 * 3600LL;
    CHECK_EQ(mdDetail::tsToEpochSeconds("2025-06-01T22:00:00.065308005Z"), expected);
    CHECK_EQ(mdDetail::tsToEpochSeconds("2025-06-01T22:01:30.000000000Z"), expected + 90);
}

TEST(endTimestampAddsSeconds) {
    useFixtureMapping();
    CHECK_EQ(mdDetail::endTimestamp("2025-06-01T22:00:00.000000000Z", 60), "2025-06-01T22:01:00");
    CHECK_EQ(mdDetail::endTimestamp("2025-06-01T22:59:30.000000000Z", 60), "2025-06-01T23:00:30");
}

TEST(endTimestampRollsOverMidnight) {
    useFixtureMapping();
    CHECK_EQ(mdDetail::endTimestamp("2025-06-01T23:59:30.000000000Z", 60), "2025-06-02T00:00:30");
    // leap day boundary
    CHECK_EQ(mdDetail::endTimestamp("2024-02-28T23:59:00.000000000Z", 120), "2024-02-29T00:01:00");
}

// the Parquet backend formats typed timestamps back into this exact layout so
// Tick::timestamp stays a string_view for both backends, so the two have to agree
TEST(formatIsoTimestampRoundTripsThroughTsToEpochSeconds) {
    useFixtureMapping();
    long long secs = mdDetail::civilToDays(2025, 6, 1) * 86400LL + 22 * 3600LL + 34 * 60LL + 56LL;
    char buf[40];
    int n = mdDetail::formatIsoTimestamp(buf, sizeof(buf), secs * 1000000000LL + 65308005LL);
    CHECK_EQ(std::string(buf, (size_t)n), "2025-06-01T22:34:56.065308005Z");
    CHECK_EQ(mdDetail::tsToEpochSeconds(std::string_view(buf, (size_t)n)), secs);
}

// ---------------------------------------------------------------- nextTick

TEST(nextTickParsesPriceSizeAndTimestamp) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->tsRecv, "2025-06-01T22:00:00.000000000Z");
    CHECK_EQ(t->price, "5000.25");
    CHECK_F(t->size, 3.0);
}

TEST(nextTickSkipsHeaderRow) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());
    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_NE(t->price, "price"); // would be the header if it leaked through
}

TEST(nextTickClassifiesAggressorSide) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto buy = md.nextTick();     // side B
    REQUIRE(buy.has_value());
    CHECK_F(buy->executedBuys,  3.0);
    CHECK_F(buy->executedSells, 0.0);
    CHECK_F(buy->unknownVolume, 0.0);

    auto sell = md.nextTick();    // side A
    REQUIRE(sell.has_value());
    CHECK_F(sell->executedBuys,  0.0);
    CHECK_F(sell->executedSells, 7.0);
    CHECK_F(sell->unknownVolume, 0.0);

    auto unk = md.nextTick();     // side X, matches neither alias
    REQUIRE(unk.has_value());
    CHECK_F(unk->executedBuys,  0.0);
    CHECK_F(unk->executedSells, 0.0);
    CHECK_F(unk->unknownVolume, 2.0);
}

TEST(nextTickVolumeSplitAlwaysSumsToSize) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());
    while (auto t = md.nextTick())
        CHECK_F(t->executedBuys + t->executedSells + t->unknownVolume, t->size);
}

TEST(nextTickAllVolumeIsUnknownWhenAggressorDisabled) {
    useFixtureMapping();
    kCSVMapping.aggressor = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick(); // side column says B, but classification is off
    REQUIRE(t.has_value());
    CHECK_F(t->executedBuys,  0.0);
    CHECK_F(t->executedSells, 0.0);
    CHECK_F(t->unknownVolume, 3.0);
}

TEST(nextTickReadsRestingBidAndAsk) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    const double wantBid[6] = {40, 41, 45, 60, 61, 30};
    const double wantAsk[6] = {55, 52, 50, 20, 19, 33};
    for (int i = 0; i < 6; i++) {
        auto t = md.nextTick();
        REQUIRE(t.has_value());
        CHECK_F(t->restingBids, wantBid[i]);
        CHECK_F(t->restingAsks, wantAsk[i]);
    }
}

TEST(nextTickRestingSizesAreZeroWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.restingBidCol = -1;
    kCSVMapping.restingAskCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(t->restingBids, 0.0);
    CHECK_F(t->restingAsks, 0.0);
    CHECK_F(t->size, 3.0); // the rest of the row still parses
}

// one side mapped and the other not has to work, they are independent columns
TEST(nextTickRestingBidMappedAskUnmapped) {
    useFixtureMapping();
    kCSVMapping.restingAskCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(t->restingBids, 40.0);
    CHECK_F(t->restingAsks, 0.0);
}

TEST(nextTickReadsBidAndAskPrice) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    const double wantBid[6] = {5000.00, 5000.25, 5000.50, 5000.75, 5000.25, 5000.00};
    const double wantAsk[6] = {5000.50, 5000.75, 5001.00, 5001.25, 5000.75, 5000.50};
    for (int i = 0; i < 6; i++) {
        auto t = md.nextTick();
        REQUIRE(t.has_value());
        CHECK_F(t->bidPrice, wantBid[i]);
        CHECK_F(t->askPrice, wantAsk[i]);
    }
}

TEST(nextTickBidAskPriceAreZeroWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.bidPriceCol = -1;
    kCSVMapping.askPriceCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(t->bidPrice, 0.0);
    CHECK_F(t->askPrice, 0.0);
    CHECK_F(t->restingBids, 40.0); // the rest of the row still parses
}

// independent columns, like the resting sizes
TEST(nextTickBidPriceMappedAskPriceUnmapped) {
    useFixtureMapping();
    kCSVMapping.askPriceCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(t->bidPrice, 5000.00);
    CHECK_F(t->askPrice, 0.0);
}

TEST(nextTickReturnsNulloptAtEof) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());
    for (int i = 0; i < 6; i++) CHECK(md.nextTick().has_value());
    CHECK(!md.nextTick().has_value());
    CHECK(!md.nextTick().has_value()); // still nullopt, no walking off the end
}

// ---------------------------------------------------------------- nextClose

TEST(nextCloseSumsVolumeAcrossBar) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar1 = md.nextClose(60);
    REQUIRE(bar1.has_value());
    CHECK_F(bar1->size, 17.0); // 3 + 7 + 2 + 5

    auto bar2 = md.nextClose(60);
    REQUIRE(bar2.has_value());
    CHECK_F(bar2->size, 10.0); // 4 + 6

    CHECK(!md.nextClose(60).has_value());
}

TEST(nextCloseSumsAggressorSplitAcrossBar) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar = md.nextClose(60);
    REQUIRE(bar.has_value());
    CHECK_F(bar->executedBuys,  8.0); // 3 + 5
    CHECK_F(bar->executedSells, 7.0);
    CHECK_F(bar->unknownVolume, 2.0);
    CHECK_F(bar->executedBuys + bar->executedSells + bar->unknownVolume, bar->size);
}

TEST(nextCloseReportsClosingRowPrice) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar1 = md.nextClose(60);
    REQUIRE(bar1.has_value());
    CHECK_EQ(bar1->price, "5001.00");
    CHECK_EQ(bar1->tsRecv, "2025-06-01T22:01:05.000000000Z");

    auto bar2 = md.nextClose(60);
    REQUIRE(bar2.has_value());
    CHECK_EQ(bar2->price, "5000.25");
}

// the whole point of the resting columns: they are a book snapshot, so a bar
// reports the closing row's book rather than a sum or an average. bar 1's rows
// carry bids 40/41/45/60, if this ever starts summing it would read 186
TEST(nextCloseRestingSizesComeFromClosingRowNotSummed) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar1 = md.nextClose(60);
    REQUIRE(bar1.has_value());
    CHECK_F(bar1->restingBids, 60.0);
    CHECK_F(bar1->restingAsks, 20.0);

    auto bar2 = md.nextClose(60);
    REQUIRE(bar2.has_value());
    CHECK_F(bar2->restingBids, 30.0);
    CHECK_F(bar2->restingAsks, 33.0);
}

// same snapshot rule as the resting sizes: bar 1's rows carry bids
// 5000.00/.25/.50/.75, a sum or average would not land on the close's 5000.75
TEST(nextCloseBidAskPriceComeFromClosingRow) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar1 = md.nextClose(60);
    REQUIRE(bar1.has_value());
    CHECK_F(bar1->bidPrice, 5000.75);
    CHECK_F(bar1->askPrice, 5001.25);

    auto bar2 = md.nextClose(60);
    REQUIRE(bar2.has_value());
    CHECK_F(bar2->bidPrice, 5000.00);
    CHECK_F(bar2->askPrice, 5000.50);
}

TEST(nextCloseBidAskPriceAreZeroWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.bidPriceCol = -1;
    kCSVMapping.askPriceCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar = md.nextClose(60);
    REQUIRE(bar.has_value());
    CHECK_F(bar->bidPrice, 0.0);
    CHECK_F(bar->askPrice, 0.0);
    CHECK_F(bar->size, 17.0); // aggregation still works
}

TEST(nextCloseRestingSizesAreZeroWhenUnmapped) {
    useFixtureMapping();
    kCSVMapping.restingBidCol = -1;
    kCSVMapping.restingAskCol = -1;
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar = md.nextClose(60);
    REQUIRE(bar.has_value());
    CHECK_F(bar->restingBids, 0.0);
    CHECK_F(bar->restingAsks, 0.0);
    CHECK_F(bar->size, 17.0); // aggregation still works
}

// a bar wide enough to cover everything should collapse the file into one row
TEST(nextCloseWideBarSwallowsWholeFile) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar = md.nextClose(3600);
    REQUIRE(bar.has_value());
    CHECK_F(bar->size, 27.0);          // every row
    CHECK_EQ(bar->price, "5000.25");   // last row
    CHECK_F(bar->restingBids, 30.0);   // last row's book
    CHECK(!md.nextClose(3600).has_value());
}

// ---------------------------------------------------------------- symbol filtering

static std::string mixedSymbols() {
    return std::string(azt::kFixtureHeader) +
        "2025-06-01T22:00:00.000000000Z,ESM5,5000.25,3,B,40,55\n"
        "2025-06-01T22:00:01.000000000Z,NQM5,18000.00,9,B,10,11\n"
        "2025-06-01T22:00:02.000000000Z,ESM5,5000.50,4,A,41,52\n"
        "2025-06-01T22:00:03.000000000Z,NQM5,18000.25,8,A,12,13\n";
}

TEST(symbolFilterSkipsOtherSymbols) {
    useFixtureMapping();
    kCSVMapping.symbolCol = 1;
    kCSVMapping.symbol    = "ESM5";

    TempCsv csv(mixedSymbols());
    MarketData md(csv.path());

    auto a = md.nextTick();
    REQUIRE(a.has_value());
    CHECK_EQ(a->price, "5000.25");

    auto b = md.nextTick();
    REQUIRE(b.has_value());
    CHECK_EQ(b->price, "5000.50"); // NQM5 row in between was skipped

    CHECK(!md.nextTick().has_value());
}

TEST(symbolFilterDisabledReadsEveryRow) {
    useFixtureMapping();
    kCSVMapping.symbolCol = -1;

    TempCsv csv(mixedSymbols());
    MarketData md(csv.path());

    int n = 0;
    while (md.nextTick()) n++;
    CHECK_EQ(n, 4);
}

// symbolRoll treats the configured symbol as a root prefix, so any contract
// month starting with "ES" matches while NQ never does
TEST(symbolRollMatchesRootPrefix) {
    useFixtureMapping();
    kCSVMapping.symbolCol  = 1;
    kCSVMapping.symbol     = "ES";
    kCSVMapping.symbolRoll = true;

    TempCsv csv(std::string(azt::kFixtureHeader) +
        "2025-06-01T22:00:00.000000000Z,ESM5,5000.25,3,B,40,55\n"
        "2025-06-01T22:00:01.000000000Z,NQM5,18000.00,9,B,10,11\n"
        "2025-06-01T22:00:02.000000000Z,ESM5,5000.50,4,A,41,52\n");
    MarketData md(csv.path());

    auto a = md.nextTick();
    REQUIRE(a.has_value());
    CHECK_EQ(a->price, "5000.25");

    auto b = md.nextTick();
    REQUIRE(b.has_value());
    CHECK_EQ(b->price, "5000.50"); // NQ does not share the ES root

    CHECK(!md.nextTick().has_value());
}

// ---------------------------------------------------------------- cursor

TEST(seekToRewindsToAPreviousOffset) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    md.nextTick();                       // consumes header + row 0
    std::size_t mark = md.byteOffset();
    auto first = md.nextTick();
    REQUIRE(first.has_value());
    CHECK_EQ(first->price, "5000.50");

    md.seekTo(mark);
    auto again = md.nextTick();
    REQUIRE(again.has_value());
    CHECK_EQ(again->price, "5000.50");   // same row a second time
}

TEST(setCursorSeeksByRowCount) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto row0 = md.nextTick();
    REQUIRE(row0.has_value());
    auto row1 = md.nextTick();
    REQUIRE(row1.has_value());
    CHECK_NE(row0->price, row1->price);

    md.setCursor(1); // back to row 1, by row count rather than byte offset
    auto again = md.nextTick();
    REQUIRE(again.has_value());
    CHECK_EQ(again->price, row1->price);

    md.setCursor(0); // and all the way back to row 0
    auto again0 = md.nextTick();
    REQUIRE(again0.has_value());
    CHECK_EQ(again0->price, row0->price);
}

// skipLine runs _skipHeaderOnce() first, so on a fresh reader the FIRST call
// eats the header and a data row, same "header skipped on first call" rule
// nextTick follows. later calls drop one row each
TEST(skipLineAdvancesWithoutParsing) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    CHECK(md.skipLine()); // header + row 0
    CHECK(md.skipLine()); // row 1
    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->price, "5000.75"); // row 2
}

TEST(skipLineReturnsFalseAtEof) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    int skipped = 0;
    while (md.skipLine()) skipped++;
    CHECK_EQ(skipped, 6); // header folded into the first call, then 5 more rows
    CHECK(!md.skipLine());
}

TEST(nextCloseReportsOHLC) {
    useFixtureMapping();
    TempCsv csv(azt::basicTicks());
    MarketData md(csv.path());

    auto bar1 = md.nextClose(60);
    REQUIRE(bar1.has_value());
    CHECK_F(bar1->open, 5000.25);
    CHECK_F(bar1->high, 5001.00);
    CHECK_F(bar1->low,  5000.25);

    auto bar2 = md.nextClose(60);
    REQUIRE(bar2.has_value());
    CHECK_F(bar2->open, 5000.50);
    CHECK_F(bar2->high, 5000.50);
    CHECK_F(bar2->low,  5000.25);
}
