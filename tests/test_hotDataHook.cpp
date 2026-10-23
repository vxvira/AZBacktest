// Tests for hotDataHook.h: cacheRows (narrow a csv to some columns and a slice of its rows) and loadHot
// (load a window of a file into ram at a byte offset)

#include <string>
#include <vector>

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/hotDataHook.h"

using azt::TempCsv;
using azt::TempPath;

namespace {

constexpr double kBytes = 1.0 / static_cast<double>(hot::kGB); // gb argument worth one byte

// ts,price,bid,ask rows 0..n-1, every row the same width so byte math in the tests is exact
std::string wide(int n, bool header = true) {
    std::string s = header ? "ts,price,bid,ask\n" : "";
    for (int i = 0; i < n; ++i) {
        const std::string k = std::to_string(1000 + i);
        s += k + "," + std::to_string(5000 + i) + "," + std::to_string(4000 + i) + "," + std::to_string(6000 + i) + "\n";
    }
    return s;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p < text.size()) {
        const std::size_t nl = text.find('\n', p);
        out.push_back(text.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- cacheRows

TEST(cacheKeepsOnlyTheChosenColumns) {
    TempCsv src(wide(3));
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {0, 1};
    CHECK_EQ(hot::cacheRows(src.path(), dst.path(), o), (std::size_t)3);

    auto l = lines(azt::slurp(dst.path()));
    REQUIRE(l.size() == 4);
    CHECK_EQ(l[0], std::string("ts,price"));
    CHECK_EQ(l[1], std::string("1000,5000"));
    CHECK_EQ(l[3], std::string("1002,5002"));
}

TEST(cacheColumnsComeOutInTheRequestedOrder) {
    TempCsv src(wide(2));
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {3, 0};
    hot::cacheRows(src.path(), dst.path(), o);
    auto l = lines(azt::slurp(dst.path()));
    CHECK_EQ(l[0], std::string("ask,ts"));
    CHECK_EQ(l[1], std::string("6000,1000"));
}

TEST(cacheWithNoColumnsCopiesEverything) {
    TempCsv src(wide(4));
    TempPath dst;
    CHECK_EQ(hot::cacheRows(src.path(), dst.path()), (std::size_t)4);
    CHECK_EQ(azt::slurp(dst.path()), wide(4));
}

TEST(cacheKeepFractionTakesTheStartOfTheFile) {
    TempCsv src(wide(100));
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {0, 1};
    o.keep = 0.25;
    const std::size_t n = hot::cacheRows(src.path(), dst.path(), o);
    CHECK(n >= 25 && n <= 27); // a quarter of the bytes, finished onto a whole row

    auto l = lines(azt::slurp(dst.path()));
    CHECK_EQ(l.size(), n + 1);
    CHECK_EQ(l[1], std::string("1000,5000")); // starts at the first row
    for (std::size_t i = 1; i < l.size(); ++i) CHECK_EQ(l[i], std::to_string(1000 + i - 1) + "," + std::to_string(5000 + i - 1));
}

TEST(cacheFromMovesTheSliceAndNeverSplitsARow) {
    TempCsv src(wide(100));
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {0};
    o.from = 0.5;
    o.keep = 0.25;
    const std::size_t n = hot::cacheRows(src.path(), dst.path(), o);
    CHECK(n >= 24 && n <= 27);

    auto l = lines(azt::slurp(dst.path()));
    REQUIRE(l.size() == n + 1);
    const int first = std::stoi(l[1]);
    CHECK(first >= 1050 && first <= 1051); // about halfway
    for (std::size_t i = 1; i < l.size(); ++i) CHECK_EQ(l[i], std::to_string(first + (int)i - 1)); // contiguous, whole rows
}

TEST(cacheSliceIsClampedAtTheEndOfTheFile) {
    TempCsv src(wide(10));
    TempPath dst;
    hot::CacheOptions o;
    o.from = 0.8;
    o.keep = 0.9;
    const std::size_t n = hot::cacheRows(src.path(), dst.path(), o);
    CHECK(n >= 2 && n <= 3);
    auto l = lines(azt::slurp(dst.path()));
    CHECK_EQ(l.back(), std::string("1009,5009,4009,6009"));
}

TEST(cacheKeepZeroWritesOnlyTheHeader) {
    TempCsv src(wide(10));
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {1};
    o.keep = 0.0;
    CHECK_EQ(hot::cacheRows(src.path(), dst.path(), o), (std::size_t)0);
    CHECK_EQ(azt::slurp(dst.path()), std::string("price\n"));
}

TEST(cacheHeaderlessSourceHasNoHeaderInTheOutput) {
    TempCsv src(wide(3, false));
    TempPath dst;
    hot::CacheOptions o;
    o.in.header = false;
    o.columns = {1};
    CHECK_EQ(hot::cacheRows(src.path(), dst.path(), o), (std::size_t)3);
    CHECK_EQ(azt::slurp(dst.path()), std::string("5000\n5001\n5002\n"));
}

TEST(cacheHonoursDelimitersAndQuoting) {
    TempCsv src("a;b;c\n1;\"x;y\";3\n4;5;6\n");
    TempPath dst;
    hot::CacheOptions o;
    o.in.delimiter = ';';
    o.in.quoted = true;
    o.columns = {1, 2};
    hot::cacheRows(src.path(), dst.path(), o);
    CHECK_EQ(azt::slurp(dst.path()), std::string("b,c\n\"x;y\",3\n5,6\n"));
}

TEST(cacheRejectsBadFractions) {
    TempCsv src(wide(2));
    TempPath dst;
    hot::CacheOptions o;
    o.keep = 1.5;
    bool threw = false;
    try { hot::cacheRows(src.path(), dst.path(), o); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    o.keep = 0.5; o.from = -0.1;
    threw = false;
    try { hot::cacheRows(src.path(), dst.path(), o); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST(cacheOfAMissingFileThrows) {
    TempPath dst;
    bool threw = false;
    try { hot::cacheRows("no_such_file_anywhere.csv", dst.path()); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ---------------------------------------------------------------- loadHot

TEST(loadHotReadsAWholeSmallFile) {
    const std::string text = wide(5);
    TempCsv src(text);
    auto h = hot::loadHot(src.path(), 1.0);
    CHECK_EQ(h.buffer, text);
    CHECK_EQ(h.offset, (std::size_t)0);
    CHECK_EQ(h.next, text.size());
    CHECK(h.atEnd());

    auto rd = h.reader();
    int n = 0;
    while (rd.next()) ++n;
    CHECK_EQ(n, 5); // header skipped on a window that starts at byte 0
}

TEST(loadHotWindowEndsOnARowBoundary) {
    const std::string text = wide(10);
    TempCsv src(text);
    const std::size_t rowLen = std::string("1000,5000,4000,6000\n").size();
    const std::size_t head = std::string("ts,price,bid,ask\n").size();

    auto h = hot::loadHot(src.path(), (head + 3 * rowLen + 5) * kBytes); // 3 rows and a bit of the 4th
    CHECK_EQ(h.buffer.size(), head + 3 * rowLen);
    CHECK_EQ(h.next, head + 3 * rowLen);
    CHECK(!h.atEnd());
    CHECK_EQ(h.buffer.back(), '\n');
}

TEST(loadHotWalksAFileInChunksWithoutLosingOrRepeatingRows) {
    const std::string text = wide(50);
    TempCsv src(text);

    std::vector<std::string> seen;
    std::size_t at = 0;
    int windows = 0;
    for (;;) {
        auto h = hot::loadHot(src.path(), 120 * kBytes, at);
        if (h.empty()) break;
        ++windows;
        auto rd = h.reader();
        while (auto r = rd.next()) seen.emplace_back(r->raw());
        at = h.next;
        if (h.atEnd()) break;
    }
    CHECK(windows > 3);
    REQUIRE(seen.size() == 50);
    for (int i = 0; i < 50; ++i)
        CHECK_EQ(seen[i], std::to_string(1000 + i) + "," + std::to_string(5000 + i) + "," + std::to_string(4000 + i) + "," + std::to_string(6000 + i));
}

TEST(loadHotOffsetOnARowStartKeepsThatRow) {
    TempCsv src(wide(10));
    const std::size_t head = std::string("ts,price,bid,ask\n").size();
    const std::size_t rowLen = std::string("1000,5000,4000,6000\n").size();

    auto h = hot::loadHot(src.path(), 1.0, head + 2 * rowLen);
    CHECK_EQ(h.offset, head + 2 * rowLen);
    auto rd = h.reader();
    auto r = rd.next();
    REQUIRE(r.has_value());
    CHECK_EQ(r->raw(), std::string("1002,5002,4002,6002")); // no header skipped, offset > 0
}

TEST(loadHotOffsetInsideARowSkipsToTheNextOne) {
    TempCsv src(wide(10));
    const std::size_t head = std::string("ts,price,bid,ask\n").size();
    const std::size_t rowLen = std::string("1000,5000,4000,6000\n").size();

    auto h = hot::loadHot(src.path(), 1.0, head + 2 * rowLen + 7);
    CHECK_EQ(h.offset, head + 3 * rowLen);
    auto rd = h.reader();
    CHECK_EQ(rd.next()->raw(), std::string("1003,5003,4003,6003"));
}

TEST(loadHotOffsetMovesBetweenCalls) {
    TempCsv src(wide(20));
    const std::size_t head = std::string("ts,price,bid,ask\n").size();
    const std::size_t rowLen = std::string("1000,5000,4000,6000\n").size();
    const double twoRows = 2 * rowLen * kBytes;

    auto a = hot::loadHot(src.path(), twoRows, head + 10 * rowLen);
    auto b = hot::loadHot(src.path(), twoRows, head + 4 * rowLen);
    auto c = hot::loadHot(src.path(), twoRows, head + 10 * rowLen); // same offset again, same window
    CHECK_EQ(lines(a.buffer)[0], std::string("1010,5010,4010,6010"));
    CHECK_EQ(lines(b.buffer)[0], std::string("1004,5004,4004,6004"));
    CHECK_EQ(a.buffer, c.buffer);
    CHECK_EQ(a.next, head + 12 * rowLen);
}

TEST(loadHotPastTheEndIsEmpty) {
    const std::string text = wide(3);
    TempCsv src(text);
    auto h = hot::loadHot(src.path(), 1.0, text.size() + 100);
    CHECK(h.empty());
    CHECK(h.atEnd());
    auto h2 = hot::loadHot(src.path(), 1.0, text.size());
    CHECK(h2.empty());
}

TEST(loadHotLastWindowReachesTheEndOfTheFile) {
    const std::string text = wide(10);
    TempCsv src(text);
    const std::size_t rowLen = std::string("1000,5000,4000,6000\n").size();
    auto h = hot::loadHot(src.path(), 1.0, text.size() - 2 * rowLen);
    CHECK(h.atEnd());
    CHECK_EQ(lines(h.buffer).size(), (std::size_t)2);
    CHECK_EQ(lines(h.buffer).back(), std::string("1009,5009,4009,6009"));
}

TEST(loadHotFileWithoutATrailingNewline) {
    std::string text = wide(4);
    text.pop_back();
    TempCsv src(text);
    auto h = hot::loadHot(src.path(), 1.0);
    CHECK_EQ(h.buffer, text);
    CHECK(h.atEnd());
}

TEST(loadHotRejectsBadArguments) {
    TempCsv src(wide(3));
    bool threw = false;
    try { hot::loadHot(src.path(), 0.0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { hot::loadHot("no_such_file_anywhere.csv", 1.0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    threw = false; // a window that can't hold even one row is an error, not a silent empty
    try { hot::loadHot(src.path(), 3 * kBytes, 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(loadHotReadsACacheFileMadeByCacheRows) {
    TempCsv src(wide(40));
    TempPath cache;
    hot::CacheOptions o;
    o.columns = {0, 1};
    o.keep = 0.5;
    const std::size_t n = hot::cacheRows(src.path(), cache.path(), o);

    auto h = hot::loadHot(cache.path(), 1.0);
    CHECK(h.atEnd());
    auto rd = h.reader();
    std::size_t rows = 0;
    while (auto r = rd.next()) { CHECK_EQ(r->size(), 2); ++rows; }
    CHECK_EQ(rows, n);
}

// ---------------------------------------------------------------- other formats

#if defined(AZBT_DUCKDB) || defined(AZBT_PARQUET)
namespace {

std::string timeOf(int i) {
    char b[32];
    std::snprintf(b, sizeof b, "2025-06-01T22:00:%02d.000000000Z", i);
    return b;
}

// ts,price,size rows 0..n-1 as the csv the other formats should come out as
std::string expectedCsv(int n) {
    std::string s = "ts,price,size\n";
    for (int i = 0; i < n; ++i) s += timeOf(i) + "," + std::to_string(5000 + i) + ".5," + std::to_string(i) + "\n";
    return s;
}

} // namespace
#endif

#ifdef AZBT_DUCKDB
namespace {

class DuckFile {
    std::string _path;
public:
    explicit DuckFile(int n) {
        static int counter = 0;
        _path = ".azt_test_hot_" + std::to_string(counter++) + ".duckdb";
        std::remove(_path.c_str());
        duckdb_database db = nullptr;
        duckdb_connection con = nullptr;
        REQUIRE(duckdb_open(_path.c_str(), &db) == DuckDBSuccess);
        duckdb_connect(db, &con);
        duckdb_result r;
        const std::string sql = "CREATE TABLE t AS SELECT TIMESTAMP_NS '2025-06-01 22:00:00' + to_seconds(i) AS ts, "
            "i + 5000.5 AS price, i AS size FROM range(" + std::to_string(n) + ") r(i)";
        const bool ok = duckdb_query(con, sql.c_str(), &r) == DuckDBSuccess;
        if (!ok) std::printf("        duckdb: %s\n", duckdb_result_error(&r));
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);
        REQUIRE(ok);
    }
    ~DuckFile() { std::remove(_path.c_str()); std::remove((_path + ".wal").c_str()); }
    const std::string& path() const { return _path; }
};

} // namespace

TEST(cacheReadsADuckdbTable) {
    DuckFile db(10);
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {0, 1};
    CHECK_EQ(hot::cacheRows(db.path(), dst.path(), o), (std::size_t)10);
    auto l = lines(azt::slurp(dst.path()));
    REQUIRE(l.size() == 11);
    CHECK_EQ(l[0], std::string("ts,price"));
    CHECK_EQ(l[1], timeOf(0) + ",5000.5");
    CHECK_EQ(l[10], timeOf(9) + ",5009.5");
}

TEST(cacheReadsADuckdbQueryAndKeepsASlice) {
    DuckFile db(100);
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {2};
    o.keep = 0.5;
    const std::size_t n = hot::cacheRows(db.path() + "#SELECT * FROM t WHERE size >= 20", dst.path(), o);
    CHECK(n >= 38 && n <= 42);
    auto l = lines(azt::slurp(dst.path()));
    CHECK_EQ(l[1], std::string("20"));
}

TEST(loadHotReadsADuckdbFileAndMovesOffsets) {
    DuckFile db(30);
    const std::string text = expectedCsv(30);
    auto all = hot::loadHot(db.path(), 1.0);
    CHECK_EQ(all.buffer, text);
    CHECK(all.atEnd());

    const std::size_t head = std::string("ts,price,size\n").size();
    const std::size_t rowLen = (timeOf(0) + ",5000.5,0\n").size();
    auto a = hot::loadHot(db.path(), 3 * rowLen * kBytes, head + 5 * rowLen);
    CHECK_EQ(lines(a.buffer)[0], timeOf(5) + ",5005.5,5");
    CHECK_EQ(lines(a.buffer).size(), (std::size_t)3);
    auto b = hot::loadHot(db.path(), 3 * rowLen * kBytes, a.next);
    CHECK_EQ(lines(b.buffer)[0], timeOf(8) + ",5008.5,8");
}

TEST(loadHotDuckdbWindowsCoverTheTable) {
    DuckFile db(25);
    std::size_t at = 0, rows = 0;
    for (;;) {
        auto h = hot::loadHot(db.path(), 200 * kBytes, at);
        if (h.empty()) break;
        auto rd = h.reader();
        while (rd.next()) ++rows;
        at = h.next;
        if (h.atEnd()) break;
    }
    CHECK_EQ(rows, (std::size_t)25);
}
#else
TEST(duckdbSourceWithoutSupportThrows) {
    TempPath dst;
    bool threw = false;
    try { hot::cacheRows("x.duckdb", dst.path()); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
#endif

#ifdef AZBT_PARQUET
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

namespace {

class ParquetFile {
    std::string _path;
public:
    explicit ParquetFile(int n, int64_t groupRows = 7) {
        static int counter = 0;
        _path = ".azt_test_hot_" + std::to_string(counter++) + ".parquet";
        arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
        arrow::DoubleBuilder px;
        arrow::Int64Builder sz;
        const long long base = mdDetail::civilToDays(2025, 6, 1) * 86400LL + 22 * 3600LL;
        for (int i = 0; i < n; ++i) {
            REQUIRE(ts.Append((base + i) * 1000000000LL).ok());
            REQUIRE(px.Append(5000.5 + i).ok());
            REQUIRE(sz.Append(i).ok());
        }
        std::shared_ptr<arrow::Array> a, b, c;
        REQUIRE(ts.Finish(&a).ok());
        REQUIRE(px.Finish(&b).ok());
        REQUIRE(sz.Finish(&c).ok());
        auto schema = arrow::schema({arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO)),
                                     arrow::field("price", arrow::float64()), arrow::field("size", arrow::int64())});
        auto table = arrow::Table::Make(schema, {a, b, c});
        auto sink = arrow::io::FileOutputStream::Open(_path);
        REQUIRE(sink.ok());
        REQUIRE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *sink, groupRows).ok());
        REQUIRE((*sink)->Close().ok());
    }
    ~ParquetFile() { std::remove(_path.c_str()); }
    const std::string& path() const { return _path; }
};

} // namespace

TEST(cacheReadsAParquetFile) {
    ParquetFile pq(20);
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {0, 2};
    CHECK_EQ(hot::cacheRows(pq.path(), dst.path(), o), (std::size_t)20);
    auto l = lines(azt::slurp(dst.path()));
    REQUIRE(l.size() == 21);
    CHECK_EQ(l[0], std::string("ts,size"));
    CHECK_EQ(l[1], timeOf(0) + ",0");
    CHECK_EQ(l[20], timeOf(19) + ",19");
}

TEST(cacheKeepsASliceOfAParquetFile) {
    ParquetFile pq(100);
    TempPath dst;
    hot::CacheOptions o;
    o.columns = {2};
    o.from = 0.5;
    o.keep = 0.25;
    const std::size_t n = hot::cacheRows(pq.path(), dst.path(), o);
    CHECK(n >= 22 && n <= 28);
    auto l = lines(azt::slurp(dst.path()));
    const int first = std::stoi(l[1]);
    CHECK(first >= 45 && first <= 60);
    for (std::size_t i = 1; i < l.size(); ++i) CHECK_EQ(std::stoi(l[i]), first + (int)i - 1);
}

TEST(loadHotReadsAParquetFile) {
    ParquetFile pq(30);
    auto all = hot::loadHot(pq.path(), 1.0);
    CHECK_EQ(all.buffer, expectedCsv(30));

    const std::size_t head = std::string("ts,price,size\n").size();
    const std::size_t rowLen = (timeOf(0) + ",5000.5,0\n").size();
    auto a = hot::loadHot(pq.path(), (4 * rowLen + 4) * kBytes, head + 10 * rowLen); // two digit sizes, a byte longer
    CHECK_EQ(lines(a.buffer)[0], timeOf(10) + ",5010.5,10");
    CHECK_EQ(lines(a.buffer).size(), (std::size_t)4);
}
#else
TEST(parquetSourceWithoutSupportThrows) {
    TempPath dst;
    bool threw = false;
    try { hot::cacheRows("x.parquet", dst.path()); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
#endif
