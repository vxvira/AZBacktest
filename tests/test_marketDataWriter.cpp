// Unit tests for MarketDataWriter, mostly round trips: whatever it writes has
// to read straight back through MarketData with the same kCSVMapping

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"

#include <vector>

using azt::TempCsv;
using azt::TempPath;
using azt::slurp;
using azt::useFixtureMapping;

// owned copy of a Tick, the string_view fields die with the MarketData they
// came from so they can't be compared across two readers directly
struct OwnedTick {
    std::string tsRecv, price, side, action, symbol;
    double size, restingBids, restingAsks, bidPrice, askPrice;
};

static OwnedTick own(const Tick& t) {
    return { std::string(t.tsRecv), std::string(t.price), t.side, t.action, std::string(t.symbol),
             t.size, t.restingBids, t.restingAsks, t.bidPrice, t.askPrice };
}

static std::vector<OwnedTick> readAllTicks(const std::string& path) {
    std::vector<OwnedTick> out;
    MarketData md(path);
    while (auto t = md.nextTick()) out.push_back(own(*t));
    return out;
}

static void checkSame(const OwnedTick& a, const OwnedTick& b) {
    CHECK_EQ(a.tsRecv, b.tsRecv);
    CHECK_EQ(a.price, b.price);
    CHECK_EQ(a.side, b.side);
    CHECK_EQ(a.action, b.action);
    CHECK_EQ(a.symbol, b.symbol);
    CHECK_F(a.size, b.size);
    CHECK_F(a.restingBids, b.restingBids);
    CHECK_F(a.restingAsks, b.restingAsks);
    CHECK_F(a.bidPrice, b.bidPrice);
    CHECK_F(a.askPrice, b.askPrice);
}

TEST(ticksRoundTripThroughWriter) {
    useFixtureMapping();
    kCSVMapping.symbolCol = 1;
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        MarketDataWriter w(out.path());
        while (auto t = md.nextTick()) w.write(*t);
        CHECK_EQ(w.rowsWritten(), (std::size_t)6);
    }

    auto want = readAllTicks(src.path());
    auto got  = readAllTicks(out.path());
    REQUIRE(want.size() == got.size());
    for (std::size_t i = 0; i < want.size(); ++i) checkSame(got[i], want[i]);
}

// mapped columns land at their configured index with Databento style names,
// so the output has the same shape as the fixture it came from
TEST(writerUsesMappedLayoutAndHeader) {
    useFixtureMapping();
    kCSVMapping.symbolCol = 1;
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        MarketDataWriter w(out.path());
        w.write(*md.nextTick());
    }
    CHECK_EQ(slurp(out.path()), std::string(
        "ts_recv,symbol,price,size,side,bid_sz_00,ask_sz_00,bid_px_00,ask_px_00,action\n"
        "2025-06-01T22:00:00.000000000Z,ESM5,5000.25,3,B,40,55,5000,5000.5,A\n"));
}

TEST(unmappedColumnsAreLeftBlank) {
    useFixtureMapping();
    // symbol (col 1) stays unmapped, so there's a gap between ts and price
    kCSVMapping.restingBidCol = -1;
    kCSVMapping.restingAskCol = -1;
    kCSVMapping.bidPriceCol   = -1;
    kCSVMapping.askPriceCol   = -1;
    kCSVMapping.actionCol     = -1;
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        MarketDataWriter w(out.path());
        w.write(*md.nextTick());
    }
    CHECK_EQ(slurp(out.path()), std::string(
        "ts_recv,,price,size,side\n"
        "2025-06-01T22:00:00.000000000Z,,5000.25,3,B\n"));
}

TEST(noHeaderWhenSkipHeaderIsOff) {
    useFixtureMapping();
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        kCSVMapping.skipHeader = false; // only the writer sees this
        MarketDataWriter w(out.path());
        w.write(*md.nextTick());
    }
    std::string s = slurp(out.path());
    CHECK(s.rfind("2025-06-01T22:00:00", 0) == 0);

    // and the reader takes the first line as data the same way
    MarketData back(out.path());
    auto t = back.nextTick();
    REQUIRE(t.has_value());
    CHECK_F(csv::as<double>(t->price), 5000.25);
    CHECK(!back.nextTick().has_value());
}

// bars keep their summed volume and closing row snapshot on the way through
TEST(barsRoundTripThroughWriter) {
    useFixtureMapping();
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        MarketDataWriter w(out.path());
        while (auto bar = md.nextClose(60)) w.write(*bar);
        CHECK_EQ(w.rowsWritten(), (std::size_t)2);
    }

    auto bars = readAllTicks(out.path());
    REQUIRE(bars.size() == 2);
    CHECK_EQ(bars[0].tsRecv, std::string("2025-06-01T22:01:05.000000000Z"));
    CHECK_F(bars[0].size, 17.0);
    CHECK_EQ(bars[0].price, std::string("5001.00"));
    CHECK_F(bars[0].restingBids, 60.0);
    CHECK_EQ(bars[0].action, std::string("T"));
    CHECK_F(bars[1].size, 10.0);
    CHECK_EQ(bars[1].action, std::string("R"));
}

TEST(appendAddsRowsWithoutAnotherHeader) {
    useFixtureMapping();
    TempCsv src(azt::basicTicks());
    TempPath out;
    {
        MarketData md(src.path());
        MarketDataWriter first(out.path());
        first.write(*md.nextTick());
        first.close();
        MarketDataWriter second(out.path(), true);
        second.write(*md.nextTick());
        CHECK_EQ(second.rowsWritten(), (std::size_t)1);
    }
    CHECK_EQ(readAllTicks(out.path()).size(), (std::size_t)2);
}

// a tick without a symbol picks up the configured one, so a symbol filtered
// read of the output still matches
TEST(missingSymbolFallsBackToConfiguredSymbol) {
    useFixtureMapping();
    kCSVMapping.symbolCol = 1;
    kCSVMapping.symbol    = "ESM5";
    TempPath out;
    {
        MarketDataWriter w(out.path());
        Tick t;
        t.tsRecv = "2025-06-01T22:00:00.000000000Z";
        t.price  = "5000.25";
        t.size   = 1;
        w.write(t);
    }
    auto back = readAllTicks(out.path());
    REQUIRE(back.size() == 1);
    CHECK_EQ(back[0].symbol, std::string("ESM5"));
}

TEST(writeRangeOfTicks) {
    useFixtureMapping();
    TempPath out;
    std::vector<Tick> ticks(3);
    for (auto& t : ticks) { t.tsRecv = "2025-06-01T22:00:00.000000000Z"; t.price = "1"; t.size = 2; }
    {
        MarketDataWriter w(out.path());
        w.write(ticks.begin(), ticks.end());
    }
    CHECK_EQ(readAllTicks(out.path()).size(), (std::size_t)3);
}
