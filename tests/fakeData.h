// Fake market data for the test suite. Every fixture is written to a scratch
// file at runtime and deleted again when it goes out of scope, so the repo
// carries no binary/checked-in test data and each test can state the exact
// rows it cares about right next to the assertions.

#pragma once

#include "dataConfig.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace azt {

/// @brief writes `content` to a uniquely named scratch CSV in the working
/// directory and removes it on destruction
class TempCsv {
private:
    std::string _path;

public:
    explicit TempCsv(const std::string& content) {
        static int counter = 0;
        long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        _path = ".azt_test_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".csv";
        std::FILE* f = std::fopen(_path.c_str(), "wb");
        if (!f) { std::printf("        FATAL: could not create fixture %s\n", _path.c_str()); return; }
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
    ~TempCsv() { std::remove(_path.c_str()); }

    TempCsv(const TempCsv&)            = delete;
    TempCsv& operator=(const TempCsv&) = delete;

    const std::string& path() const { return _path; }
};

/// @brief reserves a uniquely named scratch path without creating anything,
/// for tests that write a file themselves. removed on destruction if it exists
class TempPath {
private:
    std::string _path;

public:
    TempPath() {
        static int counter = 0;
        long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        _path = ".azt_test_out_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".csv";
    }
    ~TempPath() { std::remove(_path.c_str()); }

    TempPath(const TempPath&)            = delete;
    TempPath& operator=(const TempPath&) = delete;

    const std::string& path() const { return _path; }
};

/// @brief whole file as a string, for checking exactly what a writer produced
inline std::string slurp(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// Canonical fixture layout shared by these tests:
//
//   col 0  ts      ISO-8601, Databento style
//   col 1  symbol
//   col 2  price
//   col 3  size
//   col 4  side    B = buy aggressor, A = sell aggressor, anything else unknown
//   col 5  bidsz   resting bid size
//   col 6  asksz   resting ask size
//   col 7  bidpx   best bid price
//   col 8  askpx   best ask price
//   col 9  action  A/C/M/T/F/R = Add/Cancel/Modify/Trade/Fill/Clear, anything else None
//
// Note the header row: useFixtureMapping() turns skipHeader on, so the reader
// drops the first line of every fixture and each one needs a header.
inline constexpr const char* kFixtureHeader = "ts,symbol,price,size,side,bidsz,asksz,bidpx,askpx,action\n";

/// @brief point the global kCSVMapping at the layout above. Call it at the top
/// of any test that reads data, then tweak individual fields for what the test
/// is actually exercising (e.g. set restingBidCol back to -1 to check the
/// unmapped path). Every test file is its own binary so this can't leak across
/// suites, but it does leak between tests in a file, hence "call it every time"
inline void useFixtureMapping() {
    cfgDetail::loaded = true; // keep loadConfig from ever touching config.toml

    kCSVMapping.tsRecvCol  = 0;
    kCSVMapping.priceCol   = 2;
    kCSVMapping.sizeCol    = 3;
    kCSVMapping.skipHeader = true;

    kCSVMapping.tsEventCol   = -1;
    kCSVMapping.rowNumberCol = -1;

    kCSVMapping.dateFormat = {0, 4, 5, 2, 8, 2}; // ISO-8601

    kCSVMapping.symbolCol  = -1;
    kCSVMapping.symbol     = "";
    kCSVMapping.symbolRoll = false;

    kCSVMapping.aggressor                 = 4;
    kCSVMapping.buySideAggressorAlias     = "B";
    kCSVMapping.sellSideAggressorAlias    = "A";
    kCSVMapping.unknownSideAggressorAlias = "N";

    kCSVMapping.restingBidCol = 5;
    kCSVMapping.restingAskCol = 6;

    kCSVMapping.bidPriceCol = 7;
    kCSVMapping.askPriceCol = 8;

    kCSVMapping.actionCol         = 9;
    kCSVMapping.actionAddAlias    = "A";
    kCSVMapping.actionCancelAlias = "C";
    kCSVMapping.actionModifyAlias = "M";
    kCSVMapping.actionTradeAlias  = "T";
    kCSVMapping.actionFillAlias   = "F";
    kCSVMapping.actionClearAlias  = "R";
    kCSVMapping.actionNoneAlias   = "N";

    kCSVMapping.flagsCol      = -1;
    kCSVMapping.bidCountCol   = -1;
    kCSVMapping.askCountCol   = -1;
    kCSVMapping.instrumentCol = -1;
    kCSVMapping.tradesOnly    = false;

    kCSVMapping.commision  = 0.f;
    kCSVMapping.spread     = 0.f;
    kCSVMapping.timingCost = 0.f;
}

/// @brief six trades spanning ~2 minutes, one of them with an unclassifiable
/// side. Laid out so that nextClose(60) produces exactly two bars:
///
///   bar 1  rows 0-3  vol 17  buys 8  sells 7  unknown 2  close 5001.00  book 60/20  quote 5000.75/5001.25  action T
///   bar 2  rows 4-5  vol 10  buys 6  sells 4  unknown 0  close 5000.25  book 30/33  quote 5000.00/5000.50  action R
///
/// the resting sizes and the quote deliberately move every row, so a test can
/// tell a close row snapshot apart from a sum or an average across the bar.
/// action cycles through all six configured aliases (A/C/M/T/F/R) across the
/// six rows, one per row, so nextTick can be checked against every alias and
/// nextClose against the two closing rows' (T, R)
inline std::string basicTicks() {
    return std::string(kFixtureHeader) +
        "2025-06-01T22:00:00.000000000Z,ESM5,5000.25,3,B,40,55,5000.00,5000.50,A\n"
        "2025-06-01T22:00:10.000000000Z,ESM5,5000.50,7,A,41,52,5000.25,5000.75,C\n"
        "2025-06-01T22:00:30.000000000Z,ESM5,5000.75,2,X,45,50,5000.50,5001.00,M\n"
        "2025-06-01T22:01:05.000000000Z,ESM5,5001.00,5,B,60,20,5000.75,5001.25,T\n"
        "2025-06-01T22:01:40.000000000Z,ESM5,5000.50,4,A,61,19,5000.25,5000.75,F\n"
        "2025-06-01T22:02:10.000000000Z,ESM5,5000.25,6,B,30,33,5000.00,5000.50,R\n";
}

} // namespace azt
