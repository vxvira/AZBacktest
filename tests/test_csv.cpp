// Unit tests for the generic csv toolkit in src/csv.h, no kCSVMapping anywhere
// in here, it's meant to stand on its own

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/csv.h"

#include <optional>

using azt::TempCsv;
using azt::TempPath;
using azt::slurp;

// ---------------------------------------------------------------- Row

TEST(rowIndexesColumns) {
    csv::Row r(std::string_view("a,bb,ccc,dddd"));
    CHECK_EQ(r[0], "a");
    CHECK_EQ(r[1], "bb");
    CHECK_EQ(r[2], "ccc");
    CHECK_EQ(r[3], "dddd"); // last field, no trailing comma
}

TEST(rowOutOfRangeAndNegativeAreEmpty) {
    csv::Row r(std::string_view("a,b"));
    CHECK_EQ(r[5], "");
    CHECK_EQ(r[-1], "");
}

TEST(rowTrailingDelimiterIsAnEmptyField) {
    csv::Row r(std::string_view("a,b,"));
    CHECK_EQ(r.size(), 3);
    CHECK_EQ(r[2], "");
}

TEST(rowSizeAndSplit) {
    csv::Row r(std::string_view("a,,c"));
    CHECK_EQ(r.size(), 3);
    auto parts = r.split();
    REQUIRE(parts.size() == 3);
    CHECK_EQ(parts[0], "a");
    CHECK_EQ(parts[1], "");
    CHECK_EQ(parts[2], "c");

    csv::Row blank(std::string_view(""));
    CHECK(blank.empty());
    CHECK_EQ(blank.size(), 0);
    CHECK(blank.split().empty());
}

TEST(rowDropsTrailingCarriageReturn) {
    csv::Row r(std::string_view("a,b,c\r"));
    CHECK_EQ(r[2], "c");
    CHECK_EQ(r.raw(), "a,b,c");
}

TEST(rowFieldsExtractsEveryRequestedColumn) {
    csv::Row r(std::string_view("a,bb,ccc,dddd,e"));
    const int cols[3] = {0, 2, 4};
    std::string_view f[3];
    r.fields(cols, 3, f);
    CHECK_EQ(f[0], "a");
    CHECK_EQ(f[1], "ccc");
    CHECK_EQ(f[2], "e");
}

// stops at whichever index is largest, not the last one, so a config can list
// columns in any order
TEST(rowFieldsHandlesUnsortedColumnIndices) {
    csv::Row r(std::string_view("a,bb,ccc,dddd,e"));
    const int cols[4] = {3, 0, 4, 1};
    std::string_view f[4];
    r.fields(cols, 4, f);
    CHECK_EQ(f[0], "dddd");
    CHECK_EQ(f[1], "a");
    CHECK_EQ(f[2], "e");
    CHECK_EQ(f[3], "bb");
}

// -1 means not wanted, it has to come back untouched rather than matching
// column 0 or walking off the row
TEST(rowFieldsLeavesNegativeColumnsUntouched) {
    csv::Row r(std::string_view("a,bb,ccc"));
    const int cols[3] = {-1, 1, -1};
    std::string_view f[3] = {"sentinel", "", "sentinel"};
    r.fields(cols, 3, f);
    CHECK_EQ(f[0], "sentinel");
    CHECK_EQ(f[1], "bb");
    CHECK_EQ(f[2], "sentinel");
}

TEST(rowFieldsRepeatedIndexFillsEverySlot) {
    csv::Row r(std::string_view("a,bb,ccc"));
    const int cols[2] = {1, 1};
    std::string_view f[2];
    r.fields(cols, 2, f);
    CHECK_EQ(f[0], "bb");
    CHECK_EQ(f[1], "bb");
}

TEST(rowFieldsShortRowLeavesMissingColumnsEmpty) {
    csv::Row r(std::string_view("a,bb"));
    const int cols[2] = {0, 6};
    std::string_view f[2];
    r.fields(cols, 2, f);
    CHECK_EQ(f[0], "a");
    CHECK_EQ(f[1], "");
}

TEST(rowGetParsesWithFallback) {
    csv::Row r(std::string_view("5000.25,3,,abc,true"));
    CHECK_F(r.get<double>(0), 5000.25);
    CHECK_EQ(r.get<int>(1), 3);
    CHECK_EQ(r.get<int>(2, -7), -7);   // empty
    CHECK_EQ(r.get<int>(3, -7), -7);   // doesnt parse
    CHECK_EQ(r.get<int>(9, -7), -7);   // past the end
    CHECK_EQ(r.get<bool>(4), true);
    CHECK_EQ(r.get<std::string>(3), std::string("abc"));
}

TEST(customDelimiter) {
    csv::Options opt;
    opt.delimiter = ';';
    csv::Row r(std::string_view("a;b,c;d"), opt);
    CHECK_EQ(r.size(), 3);
    CHECK_EQ(r[1], "b,c");

    opt.delimiter = '\t';
    csv::Row t(std::string_view("x\ty"), opt);
    CHECK_EQ(t[1], "y");
}

// ---------------------------------------------------------------- quoting

TEST(quotedModeKeepsDelimitersInsideQuotes) {
    csv::Options opt;
    opt.quoted = true;
    csv::Row r(std::string_view("1,\"hello, world\",3"), opt);
    CHECK_EQ(r.size(), 3);
    CHECK_EQ(r[1], "\"hello, world\"");
    CHECK_EQ(r.text(1), std::string("hello, world"));
    CHECK_EQ(r[2], "3");
}

TEST(plainModeIgnoresQuotes) {
    csv::Row r(std::string_view("1,\"a,b\",3"));
    CHECK_EQ(r.size(), 4);
}

TEST(unquoteCollapsesDoubledQuotes) {
    CHECK_EQ(csv::unquote("\"say \"\"hi\"\"\""), std::string("say \"hi\""));
    CHECK_EQ(csv::unquote("plain"), std::string("plain"));
    CHECK_EQ(csv::unquote("\"\""), std::string(""));
}

// ---------------------------------------------------------------- parse

TEST(parseLeavesDestinationAloneWhenEmptyOrBad) {
    double v = 7.0;
    CHECK(!csv::parse("", v));
    CHECK_F(v, 7.0);
    CHECK(!csv::parse("nope", v));
    CHECK_F(v, 7.0);
    CHECK(csv::parse("42", v));
    CHECK_F(v, 42.0);
}

TEST(parseIgnoresSurroundingSpaces) {
    CHECK_EQ(csv::as<int>("  12 "), 12);
    CHECK_F(csv::as<double>(" -1.5"), -1.5);
}

TEST(parseBoolSpellings) {
    CHECK_EQ(csv::as<bool>("1"), true);
    CHECK_EQ(csv::as<bool>("TRUE"), true);
    CHECK_EQ(csv::as<bool>("false"), false);
    CHECK_EQ(csv::as<bool>("maybe", true), true);
}

// ---------------------------------------------------------------- Reader

TEST(readerSkipsHeaderAndStreamsRows) {
    csv::Reader in(std::string_view("a,b\n1,2\n3,4\n"));
    auto r1 = in.next();
    REQUIRE(r1.has_value());
    CHECK_EQ((*r1)[0], "1");
    auto r2 = in.next();
    REQUIRE(r2.has_value());
    CHECK_EQ((*r2)[1], "4");
    CHECK(!in.next().has_value());
    CHECK(in.atEnd());
}

TEST(readerWithoutHeaderReturnsFirstLine) {
    csv::Options opt;
    opt.header = false;
    csv::Reader in(std::string_view("1,2\n3,4"), opt);
    CHECK(in.header().empty());
    auto r = in.next();
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0], "1");
    CHECK_EQ(in.rowCount(), (std::size_t)2);
}

TEST(readerLastLineWithoutNewline) {
    csv::Reader in(std::string_view("h\nx\ny"));
    int n = 0;
    while (in.next()) n++;
    CHECK_EQ(n, 2);
}

TEST(readerHandlesCrlf) {
    csv::Reader in(std::string_view("ts,px\r\n1,10.5\r\n2,11.5\r\n"));
    CHECK_EQ(in.column("px"), 1);
    auto r = in.next();
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[1], "10.5");
}

TEST(readerColumnLooksUpHeaderNames) {
    csv::Reader in(std::string_view("ts, price ,size\n"));
    CHECK_EQ(in.column("ts"), 0);
    CHECK_EQ(in.column("price"), 1); // padding around names ignored
    CHECK_EQ(in.column("size"), 2);
    CHECK_EQ(in.column("nope"), -1);
}

TEST(readerColumnUnquotesHeaderNames) {
    csv::Options opt;
    opt.quoted = true;
    csv::Reader in(std::string_view("\"a,b\",c\n"), opt);
    CHECK_EQ(in.column("a,b"), 0);
    CHECK_EQ(in.column("c"), 1);
}

TEST(readerPeekDoesntConsume) {
    csv::Reader in(std::string_view("h\n1\n2\n"));
    auto p = in.peek();
    REQUIRE(p.has_value());
    CHECK_EQ((*p)[0], "1");
    auto n = in.next();
    REQUIRE(n.has_value());
    CHECK_EQ((*n)[0], "1");
}

TEST(readerSkipAndSeekTo) {
    csv::Reader in(std::string_view("h\n1\n2\n3\n"));
    CHECK(in.skip());
    std::size_t mark = in.byteOffset();
    CHECK(in.skip());
    CHECK(in.skip());
    CHECK(!in.skip());
    in.seekTo(mark);
    auto r = in.next();
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0], "2");
}

TEST(readerSeekRowAndRowAt) {
    csv::Reader in(std::string_view("h\nr0\nr1\nr2\n"));
    CHECK_EQ(in.rowCount(), (std::size_t)3);

    in.seekRow(2);
    auto r = in.next();
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0], "r2");

    // rowAt doesnt move the cursor
    in.seekRow(0);
    auto at = in.rowAt(1);
    REQUIRE(at.has_value());
    CHECK_EQ((*at)[0], "r1");
    auto still = in.next();
    REQUIRE(still.has_value());
    CHECK_EQ((*still)[0], "r0");

    CHECK(!in.rowAt(3).has_value());
    CHECK(!in.rowAt(-1).has_value());
    in.seekRow(99);
    CHECK(in.atEnd());
}

TEST(readerRewindStartsOver) {
    csv::Reader in(std::string_view("h\n1\n2\n"));
    while (in.next()) {}
    in.rewind();
    auto r = in.next();
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0], "1");
}

// blank lines come back as empty rows rather than being skipped, so row
// numbers stay lined up with the file
TEST(readerBlankLinesAreEmptyRows) {
    csv::Reader in(std::string_view("h\n1\n\n2\n"));
    CHECK_EQ(in.rowCount(), (std::size_t)3);
    in.next();
    auto blank = in.next();
    REQUIRE(blank.has_value());
    CHECK(blank->empty());
}

TEST(readerEmptyBuffer) {
    csv::Reader in(std::string_view(""));
    CHECK(!in.next().has_value());
    CHECK_EQ(in.rowCount(), (std::size_t)0);
    CHECK_EQ(in.column("x"), -1);
}

// ---------------------------------------------------------------- FileReader

TEST(fileReaderReadsFromDisk) {
    TempCsv f("name,qty\nfoo,1\nbar,2\n");
    csv::FileReader in(f.path());
    int qty = in.column("qty");
    int total = 0;
    while (auto r = in.next()) total += r->get<int>(qty);
    CHECK_EQ(total, 3);
}

TEST(fileReaderEmptyFileIsFine) {
    TempCsv f("");
    csv::FileReader in(f.path());
    CHECK(!in.next().has_value());
}

TEST(fileReaderMissingFileThrows) {
    bool threw = false;
    try { csv::FileReader in(".azt_test_definitely_missing.csv"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ---------------------------------------------------------------- Writer

TEST(writerRowFormatsMixedTypes) {
    csv::Writer w;
    w.row("ESM5", 5000.25, 3, true, 'B');
    CHECK_EQ(w.str(), std::string("ESM5,5000.25,3,true,B\n"));
    CHECK_EQ(w.rowsWritten(), (std::size_t)1);
}

TEST(writerQuotesOnlyWhenNeeded) {
    csv::Writer w;
    w.row("plain", "has,comma", "has \"quote\"", "multi\nline");
    CHECK_EQ(w.str(), std::string("plain,\"has,comma\",\"has \"\"quote\"\"\",\"multi\nline\"\n"));
}

TEST(writerOptionalWritesBlankWhenEmpty) {
    csv::Writer w;
    std::optional<double> none, some = 1.5;
    w.row(1, none, some);
    CHECK_EQ(w.str(), std::string("1,,1.5\n"));
}

TEST(writerFixedPrecision) {
    csv::WriteOptions opt;
    opt.precision = 2;
    csv::Writer w(opt);
    w.row(1.0, 2.005, 1e300);
    // the huge one doesnt fit fixed notation in the buffer, falls back to shortest
    CHECK_EQ(w.str(), std::string("1.00,2.00,1e+300\n"));
}

TEST(writerShortestFloatsRoundTrip) {
    csv::Writer w;
    double v = 0.1 + 0.2;
    w.row(v);
    std::string s = w.str();
    s.pop_back(); // newline
    CHECK_EQ(csv::as<double>(s), v);
}

TEST(writerCustomDelimiterAndNewline) {
    csv::WriteOptions opt;
    opt.delimiter = ';';
    opt.newline = "\r\n";
    csv::Writer w(opt);
    w.row("a;b", "c,d");
    CHECK_EQ(w.str(), std::string("\"a;b\";c,d\r\n"));
}

TEST(writerFieldByFieldAndRowFrom) {
    csv::Writer w;
    w.field("x").field(1).endRow();
    std::vector<double> v{1.5, 2.5};
    w.rowFrom(v);
    w.header({"h1", "h2"});
    CHECK_EQ(w.str(), std::string("x,1\n1.5,2.5\nh1,h2\n"));
}

TEST(writerCopiesRowsAcross) {
    csv::Reader in(std::string_view("h\na,b,c\n"));
    csv::Writer w;
    w.row(*in.next());
    CHECK_EQ(w.str(), std::string("a,b,c\n"));
}

TEST(writerCopiesQuotedRowsVerbatim) {
    csv::Options opt;
    opt.quoted = true;
    csv::Reader in(std::string_view("h\n\"a,b\",c\n"), opt);
    csv::Writer w;
    w.row(*in.next());
    CHECK_EQ(w.str(), std::string("\"a,b\",c\n"));
}

TEST(writerToFileAndAppend) {
    TempPath p;
    {
        csv::Writer w(p.path());
        w.header({"a", "b"});
        w.row(1, 2);
    }
    {
        csv::WriteOptions opt;
        opt.append = true;
        csv::Writer w(p.path(), opt);
        w.row(3, 4);
    }
    CHECK_EQ(slurp(p.path()), std::string("a,b\n1,2\n3,4\n"));
}

TEST(writerCloseFinishesHalfWrittenRow) {
    TempPath p;
    csv::Writer w(p.path());
    w.field("dangling");
    w.close();
    CHECK(!w.isOpen());
    CHECK_EQ(slurp(p.path()), std::string("dangling\n"));
}

TEST(writerFlushesLargeOutput) {
    TempPath p;
    {
        csv::Writer w(p.path());
        for (int i = 0; i < 20000; ++i) w.row(i, i * 0.5);
    }
    csv::FileReader in(p.path(), [] { csv::Options o; o.header = false; return o; }());
    CHECK_EQ(in.rowCount(), (std::size_t)20000);
    auto last = in.rowAt(19999);
    REQUIRE(last.has_value());
    CHECK_EQ(last->get<int>(0), 19999);
}

TEST(writerMoveKeepsWriting) {
    TempPath p;
    {
        csv::Writer a(p.path());
        a.row(1);
        csv::Writer b(std::move(a));
        b.row(2);
        CHECK(!a.isOpen());
    }
    CHECK_EQ(slurp(p.path()), std::string("1\n2\n"));
}

TEST(writerUnopenablePathThrows) {
    bool threw = false;
    try { csv::Writer w("no_such_dir_azt/x.csv"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ---------------------------------------------------------------- round trips

TEST(writeThenReadRoundTrip) {
    TempPath p;
    {
        csv::Writer w(p.path());
        w.header({"name", "note", "px"});
        w.row("foo", "has, comma", 1.25);
        w.row("bar", "", -3.5);
    }
    csv::Options opt;
    opt.quoted = true;
    csv::FileReader in(p.path(), opt);
    auto r1 = in.next();
    REQUIRE(r1.has_value());
    CHECK_EQ(r1->text(1), std::string("has, comma"));
    CHECK_F(r1->get<double>(in.column("px")), 1.25);
    auto r2 = in.next();
    REQUIRE(r2.has_value());
    CHECK_EQ(r2->text(1), std::string(""));
    CHECK_F(r2->get<double>(2), -3.5);
}

TEST(writeColumnsAndReadColumn) {
    TempPath p;
    std::vector<long long> ts{100, 200, 300};
    std::vector<double> equity{1.5, 2.5};
    csv::writeColumns(p.path(), {"ts", "equity"}, ts, equity);
    CHECK_EQ(slurp(p.path()), std::string("ts,equity\n100,1.5\n200,2.5\n300,\n"));

    auto back = csv::readColumn<long long>(p.path(), "ts");
    CHECK_EQ(back.size(), (std::size_t)3);
    CHECK_EQ(back[2], 300LL);

    // short column runs out, the blank cell gets the fallback
    auto eq = csv::readColumn<double>(p.path(), 1, {}, -1.0);
    REQUIRE(eq.size() == 3);
    CHECK_F(eq[0], 1.5);
    CHECK_F(eq[2], -1.0);
}

TEST(readColumnStrings) {
    TempCsv f("name\nfoo\nbar\n");
    auto names = csv::readColumn<std::string>(f.path(), "name");
    REQUIRE(names.size() == 2);
    CHECK_EQ(names[1], std::string("bar"));
}

TEST(readColumnUnknownNameThrows) {
    TempCsv f("a\n1\n");
    bool threw = false;
    try { csv::readColumn<int>(f.path(), "b"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
