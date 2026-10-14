// Unit tests for DataApi::requestDataWindowBy / requestDataWindowNanos: windows that end where a key
// changes (a span of time), and the chunked return that keeps one long span from sitting in memory at once.
// Rows use epoch-second timestamps so each test states exactly where its rows fall.

#include <set>

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"
#include "src/backtestApi/backtestApi.h"

using azt::TempCsv;
using azt::useFixtureMapping;

namespace {

constexpr long long kSec  = 1'000'000'000LL;       // nanoseconds per second
constexpr long long kBase = 1'700'000'040LL;       // on a minute (28333334 * 60), not on a 600 s boundary

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

// one row per offset in seconds from kBase, price = the offset so a row can be told apart
std::string rowsAt(std::initializer_list<int> offsets) {
    std::string s = "t,px,sz\n";
    for (int o : offsets) s += std::to_string(kBase + o) + "," + std::to_string(o) + ",1\n";
    return s;
}

std::vector<int> pricesOf(const DataWindow& w) {
    std::vector<int> v;
    for (double p : w.prices) v.push_back((int)p);
    return v;
}

} // namespace

static_assert(kBase % 60 == 0, "the base has to sit on a minute so 60 s spans start on a row boundary");

TEST(nanosWindowsSplitWhereTheSpanChanges) {
    epochMapping();
    TempCsv csv(rowsAt({0, 10, 50, 60, 70, 130}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    auto a = data.requestDataWindowNanos(md, 60 * kSec);
    CHECK(pricesOf(a) == (std::vector<int>{0, 10, 50}));
    CHECK(a.spanDone);

    auto b = data.requestDataWindowNanos(md, 60 * kSec);
    CHECK(pricesOf(b) == (std::vector<int>{60, 70}));
    CHECK(b.spanKey == a.spanKey + 1);

    auto c = data.requestDataWindowNanos(md, 60 * kSec); // 130 s is two spans on, the empty one in between is skipped
    CHECK(pricesOf(c) == (std::vector<int>{130}));
    CHECK(c.spanKey == b.spanKey + 1);

    CHECK(data.requestDataWindowNanos(md, 60 * kSec).prices.empty()); // end of data
}

TEST(offsetShiftsTheGrid) {
    epochMapping();
    // 100 s spans on a grid that starts at kBase + 30: [kBase-70, kBase+30) [kBase+30, kBase+130) [kBase+130, ..)
    TempCsv csv(rowsAt({20, 40, 120, 140}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);
    const long long offset = (kBase + 30) * kSec;

    CHECK(pricesOf(data.requestDataWindowNanos(md, 100 * kSec, 0, 0, offset)) == (std::vector<int>{20}));
    CHECK(pricesOf(data.requestDataWindowNanos(md, 100 * kSec, 0, 0, offset)) == (std::vector<int>{40, 120}));
    CHECK(pricesOf(data.requestDataWindowNanos(md, 100 * kSec, 0, 0, offset)) == (std::vector<int>{140}));
}

TEST(aLongSpanComesBackInChunks) {
    epochMapping();
    TempCsv csv(rowsAt({0, 1, 2, 3, 4, 60, 61}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    auto a = data.requestDataWindowNanos(md, 60 * kSec, 0, 2);
    CHECK(pricesOf(a) == (std::vector<int>{0, 1}));
    CHECK(!a.spanDone);

    auto b = data.requestDataWindowNanos(md, 60 * kSec, 0, 2);
    CHECK(pricesOf(b) == (std::vector<int>{2, 3}));
    CHECK(!b.spanDone);
    CHECK(b.spanKey == a.spanKey);

    auto c = data.requestDataWindowNanos(md, 60 * kSec, 0, 2); // one row left in this span
    CHECK(pricesOf(c) == (std::vector<int>{4}));
    CHECK(c.spanDone);

    auto d = data.requestDataWindowNanos(md, 60 * kSec, 0, 2); // the next span starts fresh
    CHECK(pricesOf(d) == (std::vector<int>{60, 61}));
    CHECK(d.spanDone);
    CHECK(d.spanKey == a.spanKey + 1);
}

// a chunk that ends exactly on the span's last row must say the span is done, and the next call must not
// come back empty (empty means end of data)
TEST(aChunkEndingOnTheSpanEdgeIsMarkedDone) {
    epochMapping();
    TempCsv csv(rowsAt({0, 1, 60, 61}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    auto a = data.requestDataWindowNanos(md, 60 * kSec, 0, 2);
    CHECK(pricesOf(a) == (std::vector<int>{0, 1}));
    CHECK(a.spanDone);

    auto b = data.requestDataWindowNanos(md, 60 * kSec, 0, 2);
    CHECK(pricesOf(b) == (std::vector<int>{60, 61}));
    CHECK(b.spanDone); // and this one ends on the last row of the file

    CHECK(data.requestDataWindowNanos(md, 60 * kSec, 0, 2).prices.empty());
}

TEST(chunksAddUpToTheUnchunkedRows) {
    epochMapping();
    std::string rows = "t,px,sz\n";
    for (int i = 0; i < 500; ++i) rows += std::to_string(kBase + i * 7) + "," + std::to_string(i) + ",1\n";
    TempCsv csv(rows);

    std::vector<double> prices;
    DataApi plain(prices, 0.25, 0.5), chunked(prices, 0.25, 0.5);
    MarketData mdA(csv.path()), mdB(csv.path());

    DataWindow all = plain.requestDataWindow(mdA, 1000);
    REQUIRE(all.prices.size() == 500);

    std::vector<double> got;
    int windows = 0, doneCount = 0;
    for (;;) {
        DataWindow w = chunked.requestDataWindowNanos(mdB, 600 * kSec, 0, 17);
        if (w.prices.empty()) break;
        windows++;
        CHECK(w.prices.size() <= 17);
        if (w.spanDone) doneCount++;
        got.insert(got.end(), w.prices.begin(), w.prices.end());
    }
    CHECK(got == all.prices);
    CHECK(windows > 6);
    std::set<long long> spansTouched; // the grid is clock anchored, kBase isn't on a 600 s boundary
    for (int i = 0; i < 500; ++i) spansTouched.insert((kBase + i * 7) / 600);
    CHECK_EQ(doneCount, (int)spansTouched.size()); // exactly one done window per span the data touches
}

TEST(barsAreGroupedByTheirClosingRow) {
    epochMapping();
    std::string rows = "t,px,sz\n";
    for (int i = 0; i <= 600; i += 10) rows += std::to_string(kBase + i) + "," + std::to_string(i) + ",1\n";
    TempCsv csv(rows);

    std::vector<double> prices;
    DataApi plain(prices, 0.25, 0.5), spans(prices, 0.25, 0.5);
    MarketData mdA(csv.path()), mdB(csv.path());

    DataWindow all = plain.requestDataWindow(mdA, 1000, 60);
    REQUIRE(!all.prices.empty());

    std::vector<double> got;
    for (;;) {
        DataWindow w = spans.requestDataWindowNanos(mdB, 300 * kSec, 60);
        if (w.prices.empty()) break;
        // every bar of one window closes inside the same 300 s span
        for (long long t : w.tsRecv) CHECK_EQ(t / 300, w.tsRecv.front() / 300);
        got.insert(got.end(), w.prices.begin(), w.prices.end());
    }
    CHECK(got == all.prices);
}

TEST(aCustomKeyDecidesTheWindows) {
    epochMapping();
    TempCsv csv(rowsAt({0, 5, 10, 15, 20}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    // odd / even tens of seconds, a key that goes back and forth: 0,5 -> 0 | 10,15 -> 1 | 20 -> 2
    auto key = [](long long ns) { return ns / kSec / 10 - kBase / 10; };
    auto a = data.requestDataWindowBy(md, key);
    CHECK(pricesOf(a) == (std::vector<int>{0, 5}));
    auto b = data.requestDataWindowBy(md, key);
    CHECK(pricesOf(b) == (std::vector<int>{10, 15}));
    auto c = data.requestDataWindowBy(md, key);
    CHECK(pricesOf(c) == (std::vector<int>{20}));
}

TEST(aNonPositiveSpanThrows) {
    epochMapping();
    TempCsv csv(rowsAt({0}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    bool threw = false;
    try { data.requestDataWindowNanos(md, 0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST(filteredRowsDontOpenASpan) {
    epochMapping();
    kCSVMapping.filters = {cfgDetail::parseFilter("px", ">=60")};
    TempCsv csv(rowsAt({0, 10, 60, 70}));
    MarketData md(csv.path());
    std::vector<double> prices;
    DataApi data(prices, 0.25, 0.5);

    auto a = data.requestDataWindowNanos(md, 60 * kSec); // the first span has no surviving rows
    CHECK(pricesOf(a) == (std::vector<int>{60, 70}));
}
