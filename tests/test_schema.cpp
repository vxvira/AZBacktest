// Unit tests for the 1.2.0 data layer: columns by header name, epoch / date timestamps, rows that already
// are bars, [extra] columns, [filter] conditions, bad-row skipping and other delimiters.
// CSV only, same limitation as the rest of this suite.

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"
#include "src/backtestApi/backtestApi.h"

using azt::TempCsv;
using azt::useFixtureMapping;

namespace {

// the fixture layout with every optional column switched off, for files that only carry a few columns
void bareMapping(int ts, int price, int size) {
    useFixtureMapping();
    kCSVMapping.tsRecvCol = ts;
    kCSVMapping.priceCol  = price;
    kCSVMapping.sizeCol   = size;
    kCSVMapping.aggressor = -1;
    kCSVMapping.restingBidCol = kCSVMapping.restingAskCol = -1;
    kCSVMapping.bidPriceCol = kCSVMapping.askPriceCol = -1;
    kCSVMapping.actionCol = -1;
}

} // namespace

// ---------------------------------------------------------------- columns by name

TEST(namedColumnsResolveAgainstTheHeader) {
    bareMapping(-1, -1, -1);
    kCSVMapping.colNames = {{"tsRecvCol", "when"}, {"priceCol", "px"}, {"sizeCol", "qty"}};
    // deliberately not in the fixture order
    TempCsv csv("qty,junk,px,when\n"
                "3,x,100.5,2025-06-01T22:00:00.000000000Z\n");
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(std::string(t->price), std::string("100.5"));
    CHECK_F(t->size, 3.f);
    CHECK_EQ(std::string(t->tsRecv), std::string("2025-06-01T22:00:00.000000000Z"));
}

TEST(missingNamedColumnThrowsAndListsTheHeader) {
    bareMapping(0, -1, 2);
    kCSVMapping.colNames = {{"priceCol", "nope"}};
    TempCsv csv("ts,px,qty\n2025-06-01T22:00:00Z,1,1\n");

    bool threw = false;
    try { MarketData md(csv.path()); }
    catch (const std::runtime_error& e) {
        threw = true;
        const std::string what = e.what();
        CHECK(what.find("nope") != std::string::npos);
        CHECK(what.find("px") != std::string::npos); // the header is in the message
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- timestamps

TEST(epochMillisParseToSecondsAndNanos) {
    bareMapping(0, 1, 2);
    kCSVMapping.timeFormat = TimeFormat::EpochMillis;
    CHECK_EQ(mdDetail::tsToEpochSeconds("1700000000123"), 1700000000LL);
    CHECK_EQ(mdDetail::tsToEpochNanos("1700000000123"), 1700000000123LL * 1'000'000LL);
}

TEST(epochNanosKeepFullPrecision) {
    bareMapping(0, 1, 2);
    kCSVMapping.timeFormat = TimeFormat::EpochNanos;
    CHECK_EQ(mdDetail::tsToEpochSeconds("1700000000123456789"), 1700000000LL);
    CHECK_EQ(mdDetail::tsToEpochNanos("1700000000123456789"), 1700000000123456789LL);
}

TEST(epochSecondsWithAFractionParse) {
    bareMapping(0, 1, 2);
    kCSVMapping.timeFormat = TimeFormat::EpochSeconds;
    CHECK_EQ(mdDetail::tsToEpochSeconds("1700000000.75"), 1700000000LL);
}

TEST(dateOnlyTimestampsAreMidnight) {
    bareMapping(0, 1, 2);
    kCSVMapping.timeFormat = TimeFormat::Date;
    CHECK_EQ(mdDetail::tsToEpochSeconds("2025-06-01"), mdDetail::civilToDays(2025, 6, 1) * 86400LL);
}

// bars are windowed by a numeric compare when the stamps aren't ISO, 1000 ms rows -> 60 s bars
TEST(epochMillisBarsWindowNumerically) {
    bareMapping(0, 1, 2);
    kCSVMapping.timeFormat = TimeFormat::EpochMillis;
    std::string rows = "t,px,sz\n";
    for (int i = 0; i < 150; ++i)
        rows += std::to_string(1700000000000LL + i * 1000LL) + "," + std::to_string(100 + i) + ",1\n";
    TempCsv csv(rows);
    MarketData md(csv.path());

    auto b1 = md.nextClose(60);
    REQUIRE(b1.has_value());
    CHECK_F(b1->open, 100.f);
    CHECK_F(b1->high, 160.f); // the row at +60 s closes the bar
    CHECK_F(b1->size, 61.f);

    auto b2 = md.nextClose(60);
    REQUIRE(b2.has_value());
    CHECK_F(b2->open, 161.f);
}

// ---------------------------------------------------------------- rows that already are bars

TEST(rowsAreBarsPassOhlcThrough) {
    bareMapping(0, 4, 5);
    kCSVMapping.rowsAreBars = true;
    kCSVMapping.openCol = 1;
    kCSVMapping.highCol = 2;
    kCSVMapping.lowCol  = 3;
    TempCsv csv("ts,o,h,l,c,v\n"
                "2025-06-01T22:00:00Z,10,12,9,11,500\n"
                "2025-06-01T22:05:00Z,11,15,10,14,700\n");
    MarketData md(csv.path());

    auto a = md.nextClose(300); // seconds is ignored in this mode
    REQUIRE(a.has_value());
    CHECK_F(a->open, 10.f); CHECK_F(a->high, 12.f); CHECK_F(a->low, 9.f);
    CHECK_EQ(std::string(a->price), std::string("11"));
    CHECK_F(a->size, 500.f);

    auto b = md.nextClose(300);
    REQUIRE(b.has_value());
    CHECK_F(b->high, 15.f);
    CHECK(!md.nextClose(300).has_value());
}

TEST(rowsAreBarsWithoutOhlcColumnsUseTheClose) {
    bareMapping(0, 1, 2);
    kCSVMapping.rowsAreBars = true;
    TempCsv csv("ts,c,v\n2025-06-01T22:00:00Z,42,7\n");
    MarketData md(csv.path());

    auto a = md.nextClose(60);
    REQUIRE(a.has_value());
    CHECK_F(a->open, 42.f); CHECK_F(a->high, 42.f); CHECK_F(a->low, 42.f);
}

// ---------------------------------------------------------------- extras


TEST(extrasAggregateOverABar) {
    bareMapping(0, 1, 2);
    kCSVMapping.extras = {cfgDetail::parseExtra("iv_mean", "iv:mean"), cfgDetail::parseExtra("oi_last", "oi"),
                          cfgDetail::parseExtra("vol_sum", "vol:sum"), cfgDetail::parseExtra("iv_max", "iv:max")};
    kCSVMapping.colNames.clear();
    TempCsv csv("ts,px,sz,iv,oi,vol\n"
                "2025-06-01T22:00:00Z,100,1,0.20,1000,5\n"
                "2025-06-01T22:00:20Z,101,1,0.30,1100,6\n"
                "2025-06-01T22:00:40Z,102,1,0.40,1200,7\n"
                "2025-06-01T22:01:00Z,103,1,0.50,1300,8\n");
    MarketData md(csv.path());

    auto bar = md.nextClose(60);
    REQUIRE(bar.has_value());
    CHECK_NEAR(bar->extras[0], 0.35, 1e-9);  // mean of 0.2 .. 0.5
    CHECK_NEAR(bar->extras[1], 1300.0, 1e-9); // last
    CHECK_NEAR(bar->extras[2], 26.0, 1e-9);   // sum
    CHECK_NEAR(bar->extras[3], 0.5, 1e-9);    // max
}

TEST(extrasReachTheDataWindowByName) {
    bareMapping(0, 1, 2);
    kCSVMapping.extras = {cfgDetail::parseExtra("iv", "iv")};
    TempCsv csv("ts,px,sz,iv\n"
                "2025-06-01T22:00:00Z,100,1,0.2\n"
                "2025-06-01T22:00:01Z,101,1,\n"      // blank cell -> NaN, not 0
                "2025-06-01T22:00:02Z,102,1,0.4\n");
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    DataWindow w = data.requestDataWindow(md, 10, 0);
    REQUIRE(w.prices.size() == 3);
    const auto& iv = w.extra("iv");
    CHECK_NEAR(iv[0], 0.2, 1e-9);
    CHECK(std::isnan(iv[1]));
    CHECK_NEAR(iv[2], 0.4, 1e-9);

    bool threw = false;
    try { w.extra("missing"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(parseExtraSplitsTheAggregationSuffix) {
    auto a = cfgDetail::parseExtra("x", "implied_vol:mean");
    CHECK_EQ(a.header, std::string("implied_vol"));
    CHECK(a.agg == ExtraAgg::Mean);
    auto b = cfgDetail::parseExtra("y", "7");
    CHECK_EQ(b.col, 7);
    CHECK(b.agg == ExtraAgg::Last);
}

// ---------------------------------------------------------------- filters

TEST(filtersKeepOnlyMatchingRows) {
    bareMapping(0, 1, 2);
    kCSVMapping.filters = {cfgDetail::parseFilter("kind", "T"), cfgDetail::parseFilter("px", ">0")};
    TempCsv csv("ts,px,sz,kind\n"
                "2025-06-01T22:00:00Z,100,1,T\n"
                "2025-06-01T22:00:01Z,101,1,A\n"
                "2025-06-01T22:00:02Z,-5,1,T\n"
                "2025-06-01T22:00:03Z,103,1,T\n");
    MarketData md(csv.path());

    auto a = md.nextTick(); REQUIRE(a.has_value()); CHECK_EQ(std::string(a->price), std::string("100"));
    auto b = md.nextTick(); REQUIRE(b.has_value()); CHECK_EQ(std::string(b->price), std::string("103"));
    CHECK(!md.nextTick().has_value());
}

// the calendar spread rows have the root as a prefix, a "doesn't contain -" filter drops them
TEST(containsFilterDropsCalendarSpreadsBeforeTheRoll) {
    bareMapping(0, 1, 2);
    kCSVMapping.symbolCol = 3; kCSVMapping.symbol = "NQ"; kCSVMapping.symbolRoll = true;
    kCSVMapping.filters = {cfgDetail::parseFilter("sym", "!~-")};
    std::string rows = "ts,px,sz,sym\n2025-06-01T22:00:00Z,100,1,NQU6\n";
    // more consecutive spread rows than the roll threshold, without the filter these would roll the active contract
    for (int i = 0; i < 250; ++i) rows += "2025-06-01T22:00:01Z,7,1,NQU6-NQZ6\n";
    rows += "2025-06-01T22:00:02Z,101,1,NQU6\n";
    TempCsv csv(rows);
    MarketData md(csv.path());

    auto a = md.nextTick(); REQUIRE(a.has_value()); CHECK_EQ(std::string(a->price), std::string("100"));
    auto b = md.nextTick(); REQUIRE(b.has_value()); CHECK_EQ(std::string(b->price), std::string("101"));
    CHECK_EQ(std::string(b->symbol), std::string("NQU6"));
    CHECK(!md.nextTick().has_value());
}

TEST(parseFilterReadsOperatorsAndOperands) {
    auto a = cfgDetail::parseFilter("price", ">=10.5");
    CHECK(a.op == FilterOp::Ge); CHECK(a.numeric); CHECK_NEAR(a.number, 10.5, 1e-12);
    auto b = cfgDetail::parseFilter("sym", "!~-");
    CHECK(b.op == FilterOp::NotContains); CHECK_EQ(b.operand, std::string("-")); CHECK(!b.numeric);
    auto c = cfgDetail::parseFilter("3", "T");
    CHECK(c.op == FilterOp::Eq); CHECK_EQ(c.col, 3); CHECK(c.header.empty());
    auto d = cfgDetail::parseFilter("flags", "!0");
    CHECK(d.op == FilterOp::Ne); CHECK(d.numeric);
}

// ---------------------------------------------------------------- dirty data

TEST(badRowsAreSkippedAndCounted) {
    bareMapping(0, 1, 2);
    TempCsv csv("ts,px,sz\n"
                "2025-06-01T22:00:00Z,100,1\n"
                "2025-06-01T22:00:01Z,,1\n"        // empty price
                "2025-06-01T22:00:02Z,NaN,1\n"     // not a number
                "2025-06-01T22:00:03Z,N/A,1\n"
                ",104,1\n"                         // no timestamp
                "2025-06-01T22:00:05Z,105,1\n");
    MarketData md(csv.path());

    int rows = 0;
    while (md.nextTick()) rows++;
    CHECK_EQ(rows, 2);
    CHECK_EQ((int)md.skippedRows(), 4);
}

TEST(skipBadRowsOffKeepsEveryRow) {
    bareMapping(0, 1, 2);
    kCSVMapping.skipBadRows = false;
    TempCsv csv("ts,px,sz\n"
                "2025-06-01T22:00:00Z,100,1\n"
                "2025-06-01T22:00:01Z,,1\n");
    MarketData md(csv.path());

    int rows = 0;
    while (md.nextTick()) rows++;
    CHECK_EQ(rows, 2);
    CHECK_EQ((int)md.skippedRows(), 0);
}

// ---------------------------------------------------------------- delimiters

TEST(tabAndSemicolonDelimitedFilesRead) {
    for (char d : {'\t', ';', '|'}) {
        bareMapping(0, 1, 2);
        kCSVMapping.delimiter = d;
        const std::string s(1, d);
        TempCsv csv("ts" + s + "px" + s + "sz\n2025-06-01T22:00:00Z" + s + "100.25" + s + "4\n");
        MarketData md(csv.path());

        auto t = md.nextTick();
        REQUIRE(t.has_value());
        CHECK_EQ(std::string(t->price), std::string("100.25"));
        CHECK_F(t->size, 4.f);
    }
}

TEST(quotedFieldsWithDelimitersInside) {
    bareMapping(0, 1, 2);
    kCSVMapping.quoted = true;
    TempCsv csv("ts,px,sz,note\n2025-06-01T22:00:00Z,100,4,\"a, b\"\n");
    MarketData md(csv.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(t->size, 4.f);
}

// ---------------------------------------------------------------- version

TEST(versionIsDefined) {
    CHECK_EQ(std::string(AZBT_VERSION), std::string("1.2.1"));
}
