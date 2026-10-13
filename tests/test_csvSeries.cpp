// Unit tests for csv backed pool series (seriesPool csvFile / readCsv /
// reloadCsvSeries), the headless half of the console's reload, no imgui needed

#include "tests/testFramework.h"
#include "tests/fakeData.h"

#include "src/window/tooling/seriesPool.h"

using azt::TempPath;
using azt::slurp;

TEST(lineWithDataWritesItsCsv) {
    seriesPool::clear();
    TempPath p;
    seriesPool::addLine("pnl", std::vector<double>{1, 2.5, 3}).csvFile(p.path());
    CHECK_EQ(slurp(p.path()), "pnl\n1\n2.5\n3\n");
    CHECK_EQ(seriesPool::pool[0].csvPath, p.path());
}

TEST(emptySeriesReadsExistingCsv) {
    seriesPool::clear();
    TempPath p;
    seriesPool::addLine("pnl", std::vector<double>{4, 5}).csvFile(p.path());
    seriesPool::clear();

    // console side, started empty
    auto& s = seriesPool::addLine("pnl", std::vector<double>{}).csvFile(p.path());
    REQUIRE(s.cols() == 1);
    CHECK_EQ(s.rows(), 2);
    CHECK_F(s.data[0][1], 5.0);
}

TEST(emptySeriesWithNoFileStaysEmpty) {
    seriesPool::clear();
    TempPath p;
    auto& s = seriesPool::addLine("pnl", std::vector<double>{}).csvFile(p.path());
    CHECK_EQ(s.rows(), 0);
    CHECK_EQ(slurp(p.path()), ""); // didn't get created either
}

TEST(reloadPicksUpRewrittenFile) {
    seriesPool::clear();
    TempPath p;
    seriesPool::addLine("pnl", std::vector<double>{}).csvFile(p.path());

    // separate run writes the data
    {
        seriesPool::NamedSeries other{"pnl", {{7, 8, 9}}, {}, 0, {}};
        other.csvPath = p.path();
        seriesPool::writeCsv(other);
    }
    CHECK_EQ(seriesPool::reloadCsvSeries(), 1);
    REQUIRE(seriesPool::pool[0].rows() == 3);
    CHECK_F(seriesPool::pool[0].data[0][2], 9.0);
}

TEST(multiColumnRaggedRoundTrip) {
    seriesPool::clear();
    TempPath p;
    seriesPool::addLine("cloud", std::vector<std::vector<double>>{{1, 2, 3}, {4}},
                        {"a", "b"}).csvFile(p.path());
    CHECK_EQ(slurp(p.path()), "a,b\n1,4\n2,\n3,\n");

    auto& s = seriesPool::pool[0];
    s.data.clear();
    s.columnNames.clear();
    REQUIRE(seriesPool::readCsv(s));
    REQUIRE(s.cols() == 2);
    CHECK_EQ(s.data[0].size(), (size_t)3);
    CHECK_EQ(s.data[1].size(), (size_t)1);
    CHECK_EQ(s.colName(1), "b");
}

TEST(xyAndErrorBarsRoundTrip) {
    seriesPool::clear();
    TempPath xy, eb;
    seriesPool::addXYScatter("xy", std::vector<double>{1, 2}, std::vector<double>{10, 20}).csvFile(xy.path());
    seriesPool::addErrorBars("eb", std::vector<double>{5, 6}, std::vector<double>{0.5, 0.25}).csvFile(eb.path());
    CHECK_EQ(slurp(xy.path()), "x,y\n1,10\n2,20\n");
    CHECK_EQ(slurp(eb.path()), "eb,error\n5,0.5\n6,0.25\n");

    for (auto& s : seriesPool::pool) { s.data.clear(); s.errors.clear(); }
    CHECK_EQ(seriesPool::reloadCsvSeries(), 2);
    REQUIRE(seriesPool::pool[0].cols() == 2);
    CHECK_F(seriesPool::pool[0].data[1][1], 20.0);
    REQUIRE(seriesPool::pool[1].cols() == 1);
    CHECK_EQ(seriesPool::pool[1].errors.size(), (size_t)2);
    CHECK_F(seriesPool::pool[1].errors[1], 0.25);
}

TEST(heatmapShapeComesFromFile) {
    seriesPool::clear();
    TempPath p;
    seriesPool::addHeatmap("hm", std::vector<double>{1, 2, 3, 4, 5, 6}, 2, 3).csvFile(p.path());
    CHECK_EQ(slurp(p.path()), "0,1,2\n1,2,3\n4,5,6\n");

    auto& s = seriesPool::pool[0];
    s.data.clear();
    s.heatmapRows = s.heatmapCols = 0;
    REQUIRE(seriesPool::readCsv(s));
    CHECK_EQ(s.heatmapRows, 2);
    CHECK_EQ(s.heatmapCols, 3);
    CHECK_F(s.data[0][4], 5.0);
}
