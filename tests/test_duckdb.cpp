// Tests for the DuckDB data source: .duckdb / .ddb paths with an optional #table or #query, exported to a
// scratch CSV and read through the normal MarketData reader. the backend only exists when the build found
// DuckDB (AZBT_DUCKDB), without it these check the path helpers and the error a .duckdb path gives

#include <filesystem>
#include <fstream>

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/marketData.h"
#include "src/backtestApi/backtestApi.h"

using azt::useFixtureMapping;

TEST(duckdbPathsAreRecognised) {
    CHECK(mdDetail::isDuckdbPath("ticks.duckdb"));
    CHECK(mdDetail::isDuckdbPath("C:/data/ticks.ddb"));
    CHECK(mdDetail::isDuckdbPath("ticks.duckdb#mnq"));
    CHECK(mdDetail::isDuckdbPath("ticks.duckdb#SELECT * FROM t WHERE size > 1"));
    CHECK(!mdDetail::isDuckdbPath("ticks.csv"));
    CHECK(!mdDetail::isDuckdbPath("ticks.parquet"));
    CHECK(!mdDetail::isDuckdbPath("ticks.csv#x.duckdb"));
}

TEST(duckdbPathSplitsTableOrQuery) {
    auto a = mdDetail::splitDuckdbPath("ticks.duckdb");
    CHECK_EQ(a.file, std::string("ticks.duckdb"));
    CHECK_EQ(a.what, std::string(""));
    auto b = mdDetail::splitDuckdbPath("ticks.duckdb#mnq");
    CHECK_EQ(b.file, std::string("ticks.duckdb"));
    CHECK_EQ(b.what, std::string("mnq"));
    auto c = mdDetail::splitDuckdbPath("ticks.duckdb#SELECT * FROM t");
    CHECK_EQ(c.what, std::string("SELECT * FROM t"));
}

#ifndef AZBT_DUCKDB

TEST(duckdbPathWithoutSupportThrowsReadableError) {
    useFixtureMapping();
    bool threw = false;
    try { MarketData md("ticks.duckdb"); }
    catch (const std::runtime_error& e) { threw = std::string(e.what()).find("DuckDB") != std::string::npos; }
    CHECK(threw);
}

#else

namespace {

// a scratch .duckdb file in the working directory, removed (with its wal) when it goes out of scope
class TempDuck {
    std::string _path;
public:
    TempDuck() {
        static int counter = 0;
        _path = ".azt_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++) + ".duckdb";
    }
    ~TempDuck() { std::remove(_path.c_str()); std::remove((_path + ".wal").c_str()); }
    const std::string& path() const { return _path; }

    // run statements against the file, then close it so the reader can open it read only
    void exec(const char* sql) const {
        duckdb_database db = nullptr;
        duckdb_connection con = nullptr;
        REQUIRE(duckdb_open(_path.c_str(), &db) == DuckDBSuccess);
        REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
        duckdb_result r;
        const bool ok = duckdb_query(con, sql, &r) == DuckDBSuccess;
        if (!ok) std::printf("        duckdb: %s\n", duckdb_result_error(&r));
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);
        REQUIRE(ok);
    }
};

// the fixture layout (ts, symbol, price, size, side, bidsz, asksz, bidpx, askpx, action) as a typed table
constexpr const char* kTableDef =
    "CREATE TABLE ticks(ts TIMESTAMP_NS, symbol VARCHAR, price DOUBLE, size INTEGER, side VARCHAR, "
    "bidsz INTEGER, asksz INTEGER, bidpx DOUBLE, askpx DOUBLE, action VARCHAR);";

constexpr const char* kTicksTable =
    "CREATE TABLE ticks(ts TIMESTAMP_NS, symbol VARCHAR, price DOUBLE, size INTEGER, side VARCHAR, "
    "bidsz INTEGER, asksz INTEGER, bidpx DOUBLE, askpx DOUBLE, action VARCHAR);"
    "INSERT INTO ticks VALUES "
    "('2025-06-01 22:00:00.065308005', 'MNQM5', 5000.25, 3, 'B', 10, 12, 5000.0, 5000.25, 'T'),"
    "('2025-06-01 22:00:00.500000000', 'MNQM5', 5000.5,  2, 'A', 11, 9,  5000.25, 5000.5, 'T'),"
    "('2025-06-01 22:00:01.000000000', 'MNQM5', 5001.0,  4, 'B', 8,  7,  5000.75, 5001.0, 'T'),"
    "('2025-06-01 22:01:05.000000000', 'MNQM5', 5002.0,  1, 'B', 6,  5,  5001.75, 5002.0, 'T');";

std::size_t scratchFiles() {
    std::size_t n = 0;
    for (auto& e : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
        if (e.path().filename().string().rfind("azbt_duckdb_", 0) == 0) ++n;
    return n;
}

} // namespace

TEST(duckdbReadsTicksFromFirstTable) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    MarketData md(db.path());

    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->tsRecv, "2025-06-01T22:00:00.065308005Z"); // nanoseconds survive
    CHECK_EQ(t->price, "5000.25");
    CHECK_F(t->size, 3.0);
    CHECK_F(t->executedBuys, 3.0);
    CHECK_F(t->restingBids, 10.0);
    CHECK_F(t->bidPrice, 5000.0);
    CHECK_EQ(std::string(t->action), std::string("T"));

    int rest = 0;
    while (md.nextTick()) ++rest;
    CHECK_EQ(rest, 3);
}

TEST(duckdbTimestampsWorkForEverySourcePrecision) {
    useFixtureMapping();
    TempDuck db;
    db.exec("CREATE TABLE a(ts TIMESTAMP, price DOUBLE, size INTEGER);"
            "INSERT INTO a VALUES ('2025-06-01 22:00:00.123456', 1.5, 1);"
            "CREATE TABLE b(ts TIMESTAMP_S, price DOUBLE, size INTEGER);"
            "INSERT INTO b VALUES ('2025-06-01 22:00:07', 2.5, 1);"
            "CREATE TABLE c(ts TIMESTAMP_MS, price DOUBLE, size INTEGER);"
            "INSERT INTO c VALUES ('2025-06-01 22:00:09.250', 3.5, 1);");
    kCSVMapping.tsRecvCol = 0; kCSVMapping.priceCol = 1; kCSVMapping.sizeCol = 2;
    kCSVMapping.aggressor = kCSVMapping.restingBidCol = kCSVMapping.restingAskCol = -1;
    kCSVMapping.bidPriceCol = kCSVMapping.askPriceCol = kCSVMapping.actionCol = -1;

    { MarketData md(db.path() + "#a"); auto t = md.nextTick(); REQUIRE(t.has_value()); CHECK_EQ(t->tsRecv, "2025-06-01T22:00:00.123456000Z"); }
    { MarketData md(db.path() + "#b"); auto t = md.nextTick(); REQUIRE(t.has_value()); CHECK_EQ(t->tsRecv, "2025-06-01T22:00:07.000000000Z"); }
    { MarketData md(db.path() + "#c"); auto t = md.nextTick(); REQUIRE(t.has_value()); CHECK_EQ(t->tsRecv, "2025-06-01T22:00:09.250000000Z"); }

    long long expected = mdDetail::civilToDays(2025, 6, 1) * 86400LL + 22 * 3600LL + 7;
    MarketData md(db.path() + "#b");
    CHECK_EQ(mdDetail::tsToEpochSeconds(md.nextTick()->tsRecv), expected);
}

TEST(duckdbPicksNamedTable) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    db.exec("CREATE TABLE zother AS SELECT * FROM ticks WHERE price > 5001;");
    MarketData first(db.path()); // no table named, first by name
    int n = 0;
    while (first.nextTick()) ++n;
    CHECK_EQ(n, 4);

    MarketData zother(db.path() + "#zother");
    auto t = zother.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->price, "5002.0");
    CHECK(!zother.nextTick().has_value());
}

TEST(duckdbRunsAQueryInsteadOfATable) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    MarketData md(db.path() + "#SELECT * FROM ticks WHERE size >= 3 ORDER BY ts");
    auto a = md.nextTick();
    REQUIRE(a.has_value());
    CHECK_F(a->size, 3.0);
    auto b = md.nextTick();
    REQUIRE(b.has_value());
    CHECK_F(b->size, 4.0);
    CHECK(!md.nextTick().has_value());

    // WITH and FROM-first queries count too
    MarketData w(db.path() + "#WITH x AS (SELECT * FROM ticks) SELECT * FROM x WHERE size = 1");
    CHECK(w.nextTick().has_value());
    MarketData f(db.path() + "#FROM ticks WHERE size = 2");
    CHECK(f.nextTick().has_value());
}

TEST(duckdbDdbExtensionWorks) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    const std::string renamed = db.path() + ".ddb";
    std::filesystem::copy_file(db.path(), renamed);
    {
        MarketData md(renamed);
        CHECK(md.nextTick().has_value());
    }
    std::remove(renamed.c_str());
}

TEST(duckdbBuildsBarsLikeCsv) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    MarketData md(db.path());

    auto bar = md.nextClose(60); // closes on the first tick past the minute, same as a CSV
    REQUIRE(bar.has_value());
    CHECK_F(bar->open, 5000.25);
    CHECK_F(bar->high, 5002.0);
    CHECK_F(bar->low, 5000.25);
    CHECK_F(bar->size, 10.0);
    CHECK_F(bar->executedBuys, 8.0);
    CHECK_F(bar->executedSells, 2.0);
}

TEST(duckdbMatchesTheEquivalentCsv) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    azt::TempCsv csv(std::string(azt::kFixtureHeader) +
        "2025-06-01T22:00:00.065308005Z,MNQM5,5000.25,3,B,10,12,5000.0,5000.25,T\n"
        "2025-06-01T22:00:00.500000000Z,MNQM5,5000.5,2,A,11,9,5000.25,5000.5,T\n"
        "2025-06-01T22:00:01.000000000Z,MNQM5,5001.0,4,B,8,7,5000.75,5001.0,T\n"
        "2025-06-01T22:01:05.000000000Z,MNQM5,5002.0,1,B,6,5,5001.75,5002.0,T\n");
    MarketData a(db.path()), b(csv.path());
    for (;;) {
        auto x = a.nextTick();
        auto y = b.nextTick();
        REQUIRE(x.has_value() == y.has_value());
        if (!x) break;
        CHECK_EQ(x->tsRecv, y->tsRecv);
        CHECK_F(std::stod(std::string(x->price)), std::stod(std::string(y->price)));
        CHECK_F(x->size, y->size);
        CHECK_EQ(std::string(x->side), std::string(y->side));
        CHECK_F(x->bidPrice, y->bidPrice);
    }
}

TEST(duckdbSymbolFilterApplies) {
    useFixtureMapping();
    kCSVMapping.symbolCol = 1;
    kCSVMapping.symbol    = "ESM5";
    TempDuck db;
    db.exec(kTableDef);
    db.exec("INSERT INTO ticks VALUES "
            "('2025-06-01 22:00:00', 'MNQM5', 1.0, 1, 'B', 1, 1, 1.0, 1.0, 'T'),"
            "('2025-06-01 22:00:01', 'ESM5',  2.0, 1, 'B', 1, 1, 1.0, 1.0, 'T');");
    MarketData md(db.path());
    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->symbol, "ESM5");
    CHECK(!md.nextTick().has_value());
}

TEST(duckdbHeaderlessConfigReadsFirstRowAsData) {
    useFixtureMapping();
    kCSVMapping.skipHeader = false;
    TempDuck db;
    db.exec(kTicksTable);
    MarketData md(db.path());
    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->price, "5000.25");
    int rest = 0;
    while (md.nextTick()) ++rest;
    CHECK_EQ(rest, 3);
}

TEST(duckdbNullsBecomeEmptyFields) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTableDef);
    db.exec("INSERT INTO ticks VALUES "
            "('2025-06-01 22:00:00', 'A', NULL, 1, 'B', 1, 1, 1.0, 1.0, 'T'),"
            "('2025-06-01 22:00:01', 'A', 7.5, 1, 'B', 1, 1, 1.0, 1.0, 'T');");
    MarketData md(db.path());
    auto t = md.nextTick(); // the NULL price row is a bad row, skipped like an empty CSV field
    REQUIRE(t.has_value());
    CHECK_EQ(t->price, "7.5");
    CHECK_EQ(md.skippedRows(), (std::size_t)1);
}

TEST(duckdbTabDelimiterConfigIsHonoured) {
    useFixtureMapping();
    kCSVMapping.delimiter = '\t';
    TempDuck db;
    db.exec(kTicksTable);
    MarketData md(db.path());
    auto t = md.nextTick();
    REQUIRE(t.has_value());
    CHECK_EQ(t->price, "5000.25");
}

TEST(duckdbMissingFileOrTableThrows) {
    useFixtureMapping();
    bool threw = false;
    try { MarketData md("no_such_file_anywhere.duckdb"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    TempDuck db;
    db.exec(kTicksTable);
    threw = false;
    try { MarketData md(db.path() + "#nope"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    TempDuck empty;
    empty.exec("CREATE VIEW v AS SELECT 1;");
    threw = false;
    try { MarketData md(empty.path()); }
    catch (const std::runtime_error& e) { threw = std::string(e.what()).find("no tables") != std::string::npos; }
    CHECK(threw);
}

TEST(duckdbScratchFileIsRemovedWithTheReader) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    const std::size_t before = scratchFiles();
    {
        MarketData md(db.path());
        CHECK_EQ(scratchFiles(), before + 1);
        md.nextTick();
    }
    CHECK_EQ(scratchFiles(), before);

    // and a failed open leaves nothing behind
    try { MarketData md(db.path() + "#nope"); } catch (const std::runtime_error&) {}
    CHECK_EQ(scratchFiles(), before);
}

TEST(duckdbGetValueReadsCells) {
    useFixtureMapping();
    TempDuck db;
    db.exec(kTicksTable);
    static std::string path;
    path = db.path();
    kCSVMapping.path = path.c_str();

    std::vector<double> prices;
    DataApi api(prices, 0.25, 0.5);
    CHECK_EQ(api.getValue(0, 1), std::string("MNQM5"));
    CHECK_EQ(api.getValue(0, 0), std::string("2025-06-01T22:00:00.065308005Z"));
    CHECK_EQ(api.getValue(2, 3), std::string("4"));
    CHECK_EQ(api.getValue(99, 0), std::string(""));
    CHECK_EQ(api.getValue(0, 99), std::string(""));
}

#endif
