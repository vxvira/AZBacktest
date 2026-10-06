// I genuinely have no clue how to do this file stuff
// so claude did (most) of it. Methods added recently are likely human written
// writing this as of september 14th 2026, for the record

#pragma once

#include "dataConfig.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

/// @brief a single row from the CSV, timestamp + price as string_views into
/// the mmapped buffer (only valid while MarketData is alive), plus a parsed
/// volume. for nextTick, `size` is that tick's traded size; for nextClose,
/// it's the summed volume across every tick in the bar
///
/// `tsRecv`, `tsEvent`, `price` and `side` always describe a single row (the
/// tick itself, or the bar's closing tick for nextClose). `size` and the
/// three volume splits are aggregates over the whole bar when it came from
/// nextClose.
///
/// executedBuys/executedSells split `size` by aggressor: buy-side volume is a
/// buyer lifting the ask, sell-side volume is a seller hitting the bid. Rows
/// whose side column matches neither alias (or when `aggressor` is -1 in the
/// config) land in unknownVolume. The three always sum to `size`.
///
/// restingBids/restingAsks are the opposite kind of number: top-of-book size
/// sitting unfilled on each side, read straight from restingBidCol/restingAskCol.
/// They're snapshots, not flow, so unlike the volume splits they're never summed
/// across a bar, nextClose reports the closing row's book the same way it reports
/// that row's price. Both stay 0 when the columns aren't configured.
///
/// bidPrice/askPrice are the best quote at the time of the row, read from
/// bidPriceCol/askPriceCol. Same snapshot rules as the resting sizes: never
/// aggregated, nextClose reports the closing row's quote, and both stay 0 when
/// the columns aren't configured.
///
/// tsEvent/rowNumber are read from tsEventCol/rowNumberCol, both optional
/// (-1 disables). Same snapshot rules again: tsEvent stays an empty
/// string_view and rowNumber stays 0 when their column isn't configured,
/// and nextClose reports the closing row's value for both.
///
/// open/high/low are bar aggregates from nextClose: the first row's price, and
/// the highest/lowest price across every row in the bar (`price` is the close).
/// nextTick leaves all three at 0, a single tick has no range.
struct Tick {
    std::string_view tsRecv;   // ts_recv: what nextClose windows bars by
    std::string_view tsEvent;  // ts_event, empty when tsEventCol is -1
    std::string_view price;
    double open = 0.0; // nextClose only, first row's price
    double high = 0.0; // nextClose only, highest price in the bar
    double low  = 0.0; // nextClose only, lowest price in the bar
    double size = 0.0;
    const char* side = kCSVMapping.unknownSideAggressorAlias;
    double executedBuys   = 0.0;
    double executedSells  = 0.0;
    double unknownVolume  = 0.0;
    double restingBids    = 0.0;
    double restingAsks    = 0.0;
    double bidPrice       = 0.0;
    double askPrice       = 0.0;
    long long rowNumber   = 0; // from rowNumberCol, 0 when not configured
    const char* action           = kCSVMapping.actionNoneAlias;
    // MBP extras, 0 / empty when their column isn't configured (and always on
    // the Parquet backend, which doesn't carry them yet)
    int flags             = 0;  // flagsCol, bit 128 = F_LAST
    double bidCount       = 0.0; // bidCountCol, resting orders at the best bid
    double askCount       = 0.0; // askCountCol, resting orders at the best ask
    long long instrumentId = 0; // instrumentCol
    std::string_view symbol;    // symbolCol, filled whether or not it filters
};

namespace mdDetail {

/// @brief Howard Hinnant's days-from-civil algorithm, portable, no DST/locale issues
/// converts a Y/M/D date to a day count since the Unix epoch
inline long long civilToDays(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

struct CivilDate { int y; unsigned m; unsigned d; };

/// @brief inverse of civilToDays, epoch day count back to Y/M/D
inline CivilDate daysToCivil(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    const unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m   = mp < 10 ? mp + 3 : mp - 9;
    return { y + (m <= 2 ? 1 : 0), m, d };
}

/// @brief parse an ISO-8601 timestamp string into epoch seconds
/// Y/M/D offsets come from dataConfig so different CSV formats just work
/// H/M/S offsets are hardcoded (they don't move around in ISO-8601)
/// @param ts the raw timestamp string_view from the CSV row
inline long long tsToEpochSeconds(std::string_view ts) {
    auto toInt = [](const char* p, int n) {
        int v = 0;
        std::from_chars(p, p + n, v);
        return v;
    };
    int y  = toInt(ts.data() + kCSVMapping.dateFormat.yearOffset,  kCSVMapping.dateFormat.yearLength);
    int mo = toInt(ts.data() + kCSVMapping.dateFormat.monthOffset, kCSVMapping.dateFormat.monthLength);
    int d  = toInt(ts.data() + kCSVMapping.dateFormat.dayOffset,   kCSVMapping.dateFormat.dayLength);
    int h  = toInt(ts.data() + 11, 2);
    int mi = toInt(ts.data() + 14, 2);
    int s  = toInt(ts.data() + 17, 2);
    return civilToDays(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL
         + h * 3600LL + mi * 60LL + s;
}

/// @brief tsToEpochSeconds plus the 9 fractional digits, full epoch nanoseconds
/// e.g. "2026-06-21T12:00:06.108130665Z"
inline long long tsToEpochNanos(std::string_view ts) {
    long long nanos = 0;
    if (ts.size() >= 29) std::from_chars(ts.data() + 20, ts.data() + 29, nanos);
    return tsToEpochSeconds(ts) * 1'000'000'000LL + nanos;
}

/// @brief build an ISO-8601 string for `startTs + seconds`, since ISO-8601
/// sorts lexicographically, nextClose can just do string compares against
/// this instead of parsing every row, way cheaper (~19-byte memcmp)
/// @param startTs the starting timestamp to offset from
/// @param seconds how far forward to go
inline std::string endTimestamp(std::string_view startTs, int seconds) {
    long long target = tsToEpochSeconds(startTs) + seconds;
    long long days = target / 86400;
    long long tod  = target % 86400;
    if (tod < 0) { tod += 86400; --days; }
    int h  = static_cast<int>(tod / 3600);
    int mi = static_cast<int>((tod / 60) % 60);
    int s  = static_cast<int>(tod % 60);
    CivilDate c = daysToCivil(days);
    char buf[24];
    int n = std::snprintf(buf, sizeof(buf),
        "%04d-%02u-%02uT%02d:%02d:%02d", c.y, c.m, c.d, h, mi, s);
    return std::string(buf, static_cast<size_t>(n));
}

/// @brief inverse of tsToEpochSeconds' string slicing: format epoch nanoseconds
/// back into Databento's ISO-8601 layout (e.g. "2025-06-01T22:00:00.065308005Z"),
/// byte-for-byte compatible with the CSV's raw format. Used by the Parquet
/// backend so Tick::timestamp stays a string_view either way, letting every
/// existing consumer (tsToEpochSeconds, endTimestamp) work unmodified.
/// @param buf caller-owned buffer, must be at least 32 bytes
/// @return number of bytes written (excluding the null terminator)
inline int formatIsoTimestamp(char* buf, std::size_t bufSize, long long nanosSinceEpoch) {
    long long days = nanosSinceEpoch / 86400000000000LL;
    long long rem  = nanosSinceEpoch % 86400000000000LL;
    if (rem < 0) { rem += 86400000000000LL; --days; }
    long long secOfDay = rem / 1000000000LL;
    long long fracNs    = rem % 1000000000LL;
    int h  = static_cast<int>(secOfDay / 3600);
    int mi = static_cast<int>((secOfDay / 60) % 60);
    int s  = static_cast<int>(secOfDay % 60);
    CivilDate c = daysToCivil(days);
    return std::snprintf(buf, bufSize, "%04d-%02u-%02uT%02d:%02d:%02d.%09lldZ",
        c.y, c.m, c.d, h, mi, s, fracNs);
}

/// @brief find first '\n' in [p, end), returns end if none found
/// memchr on modern glibc / MSVC uses SIMD (SSE2/AVX) for the scan so
/// this is basically free compared to looping byte-by-byte
inline const char* findEOL(const char* p, const char* end) {
    if (p >= end) return end;
    const void* nl = std::memchr(p, '\n', static_cast<size_t>(end - p));
    return nl ? static_cast<const char*>(nl) : end;
}

/// @brief grab the col-th (0-indexed) comma-separated field from a CSV row
/// assumes no quoted fields, fine for the numeric TBBO schema
/// @param start beginning of the row
/// @param end   one past the last char of the row (newline or EOF)
/// @param col   which column to extract
inline std::string_view field(const char* start, const char* end, int col) {
    const char* p = start;
    for (int i = 0; i < col; ++i) {
        if (p >= end) return {};
        const void* c = std::memchr(p, ',', static_cast<size_t>(end - p));
        if (!c) return {};
        p = static_cast<const char*>(c) + 1;
    }
    const void* c = (p < end)
        ? std::memchr(p, ',', static_cast<size_t>(end - p))
        : nullptr;
    const char* fieldEnd = c ? static_cast<const char*>(c) : end;
    return std::string_view(p, static_cast<size_t>(fieldEnd - p));
}

/// @brief extract two fields (cols c1 and c2, c1 <= c2) in a single pass
/// instead of calling field() twice. used by nextTick since it always
/// needs both timestamp and price from the same row
/// @param c1 first column index
/// @param c2 second column index (must be >= c1)
/// @param f1 output: view of the first field
/// @param f2 output: view of the second field
inline void twoFields(const char* start, const char* end, int c1, int c2,
                      std::string_view& f1, std::string_view& f2) {
    const char* p  = start;
    const char* fs = start;
    int col = 0;
    while (p < end) {
        if (*p == ',') {
            if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c2) { f2 = std::string_view(fs, static_cast<size_t>(p - fs)); return; }
            ++col;
            fs = p + 1;
        }
        ++p;
    }
    if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c2) f2 = std::string_view(fs, static_cast<size_t>(p - fs));
}

/// @brief three-field variant of twoFields, single-pass extraction of cols
/// c1 <= c2 <= c3, used by nextTick which needs timestamp + price + size
inline void threeFields(const char* start, const char* end, int c1, int c2, int c3,
                        std::string_view& f1, std::string_view& f2, std::string_view& f3) {
    const char* p  = start;
    const char* fs = start;
    int col = 0;
    while (p < end) {
        if (*p == ',') {
            if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c2) f2 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c3) { f3 = std::string_view(fs, static_cast<size_t>(p - fs)); return; }
            ++col;
            fs = p + 1;
        }
        ++p;
    }
    if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c2) f2 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c3) f3 = std::string_view(fs, static_cast<size_t>(p - fs));
}

/// @brief four-field variant, single-pass extraction of cols c1 <= c2 <= c3 <= c4
inline void fourFields(const char* start, const char* end, int c1, int c2, int c3, int c4,
                       std::string_view& f1, std::string_view& f2, std::string_view& f3, std::string_view& f4) {
    const char* p  = start;
    const char* fs = start;
    int col = 0;
    while (p < end) {
        if (*p == ',') {
            if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c2) f2 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c3) f3 = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == c4) { f4 = std::string_view(fs, static_cast<size_t>(p - fs)); return; }
            ++col;
            fs = p + 1;
        }
        ++p;
    }
    if (col == c1) f1 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c2) f2 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c3) f3 = std::string_view(fs, static_cast<size_t>(p - fs));
    if (col == c4) f4 = std::string_view(fs, static_cast<size_t>(p - fs));
}

/// @brief general n-column single-pass extraction, out[i] gets the field at
/// cols[i]. unlike the two/three/fourFields variants above it doesn't care what
/// order cols is in (it stops at whichever index is largest, not the last one),
/// and a negative cols[i] just leaves out[i] empty, which is what lets nextTick
/// pass optional columns like the aggressor or resting sizes straight through
/// @param cols n column indices, any order, negatives meaning "not configured"
/// @param out  caller-owned array of n views, only the ones that matched get written
inline void nFields(const char* start, const char* end, const int* cols, int n,
                    std::string_view* out) {
    int maxCol = -1;
    for (int i = 0; i < n; ++i) if (cols[i] > maxCol) maxCol = cols[i];
    if (maxCol < 0) return;

    const char* p  = start;
    const char* fs = start;
    int col = 0;
    while (p < end) {
        if (*p == ',') {
            for (int i = 0; i < n; ++i)
                if (cols[i] == col) out[i] = std::string_view(fs, static_cast<size_t>(p - fs));
            if (col == maxCol) return;
            ++col;
            fs = p + 1;
        }
        ++p;
    }
    // trailing field, no comma terminates it
    for (int i = 0; i < n; ++i)
        if (cols[i] == col) out[i] = std::string_view(fs, static_cast<size_t>(p - fs));
}

/// @brief parse a field into `dst`, leaving it untouched if the column wasn't
/// configured or the row didn't have it. keeps the "0 when disabled" default
/// that Tick's resting sizes and bid/ask prices rely on
inline void parseOptionalFloat(std::string_view v, double& dst) {
    if (v.empty()) return;
    std::from_chars(v.data(), v.data() + v.size(), dst);
}

/// @brief long long counterpart of parseOptionalFloat, used for rowNumber
inline void parseOptionalLongLong(std::string_view v, long long& dst) {
    if (v.empty()) return;
    std::from_chars(v.data(), v.data() + v.size(), dst);
}

/// @brief classify a raw action-column value against the seven configured
/// aliases, shared by both nextTick/nextClose (CSV and Parquet) so the chain
/// only lives in one place. falls back to actionNoneAlias when it matches
/// none of them, same "unknown lands on the neutral default" rule as side
inline const char* classifyAction(std::string_view actionView) {
    if (actionView == kCSVMapping.actionAddAlias)         return kCSVMapping.actionAddAlias;
    if (actionView == kCSVMapping.actionCancelAlias)      return kCSVMapping.actionCancelAlias;
    if (actionView == kCSVMapping.actionModifyAlias)      return kCSVMapping.actionModifyAlias;
    if (actionView == kCSVMapping.actionTradeAlias)       return kCSVMapping.actionTradeAlias;
    if (actionView == kCSVMapping.actionFillAlias)        return kCSVMapping.actionFillAlias;
    if (actionView == kCSVMapping.actionClearAlias)       return kCSVMapping.actionClearAlias;
    return kCSVMapping.actionNoneAlias;
}

} // namespace mdDetail

// Parquet-backed reader (_ParquetMarketData), compiled in only when build.sh
// detects Arrow/Parquet on the system (defines AZBT_PARQUET). Needs Tick and
// mdDetail::* from above, so it's included here rather than from the top of
// this file. A no-op include when AZBT_PARQUET isn't defined.
#include "parquetMarketData.h"

/// @brief streaming reader over a raw byte range, used internally by MarketData
class _MarketData {
private:
    const char* _base;
    const char* _cur;
    const char* _end;
    bool _consumedHeader = false;

    // contract roll state
    std::string _activeSymbol;
    int _missCount = 0;
    static constexpr int _rollThreshold = 200;

    std::vector<const char*> _lineIndex;

    void _buildLineIndex() {
        if (!_lineIndex.empty() || _base == _end) return;
        const char* eol = mdDetail::findEOL(_base, _end);
        const char* p = (eol < _end) ? eol + 1 : _end;
        while (p < _end) {
            _lineIndex.push_back(p);
            eol = mdDetail::findEOL(p, _end);
            p = (eol < _end) ? eol + 1 : _end;
        }
    }

    void _skipHeaderOnce() {
        if (_consumedHeader) return;
        const char* eol = mdDetail::findEOL(_cur, _end);
        _cur = (eol < _end) ? eol + 1 : _end;
        _consumedHeader = true;
    }

    // advance past rows that dont match the configured symbol filter
    // handles both exact match and rolling contract mode
    const char* _nextMatchingLine() {
        // an empty symbol means "read the column but don't filter on it"
        if (kCSVMapping.symbolCol < 0 || kCSVMapping.symbol[0] == '\0') {
            if (_cur < _end) return _cur;
            return nullptr;
        }
        std::string_view root(kCSVMapping.symbol);
        while (_cur < _end) {
            const char* line = _cur;
            const char* eol  = mdDetail::findEOL(line, _end);
            if (line == eol) return nullptr;
            std::string_view sym = mdDetail::field(line, eol, kCSVMapping.symbolCol);

            if (!kCSVMapping.symbolRoll) {
                // exact match mode
                if (sym == root) return line;
                _cur = (eol < _end) ? eol + 1 : _end;
                continue;
            }

            // rolling mode: match root prefix
            if (sym.size() < root.size() || sym.substr(0, root.size()) != root) {
                _cur = (eol < _end) ? eol + 1 : _end;
                continue;
            }

            // first contract we see becomes active
            if (_activeSymbol.empty()) {
                _activeSymbol = std::string(sym);
                _missCount = 0;
                return line;
            }

            // matches the active contract
            if (sym == std::string_view(_activeSymbol)) {
                _missCount = 0;
                return line;
            }

            // different contract, count misses and roll if the active one is gone
            _missCount++;
            if (_missCount >= _rollThreshold) {
                _activeSymbol = std::string(sym);
                _missCount = 0;
                return line;
            }
            _cur = (eol < _end) ? eol + 1 : _end;
        }
        return nullptr;
    }

public:
    _MarketData(const char* data, std::size_t size)
        : _base(data), _cur(data), _end(data + size) {}

    // skip past one line without parsing, returns false at EOF
    bool skipLine() {
        _skipHeaderOnce();
        if (_cur >= _end) return false;
        const char* eol = mdDetail::findEOL(_cur, _end);
        _cur = (eol < _end) ? eol + 1 : _end;
        return true;
    }

    std::size_t byteOffset() const { return static_cast<std::size_t>(_cur - _base); }
    void seekTo(std::size_t off) { _cur = _base + off; _consumedHeader = true; }

    /// @brief next tick from the file (header skipped on first call)
    /// @return the next tick as views into the underlying buffer, or nullopt at EOF
    std::optional<Tick> nextTick() {
        _skipHeaderOnce();
        const char* line = _nextMatchingLine();
        if (!line) return std::nullopt;
        const char* eol = mdDetail::findEOL(line, _end);
        _cur = (eol < _end) ? eol + 1 : _end;
        Tick t;
        // one pass over the row for every mapped column, the optional ones
        // (aggressor, resting sizes, bid/ask prices, ts_event, row number)
        // come back empty when their index is -1
        const int cols[16] = { kCSVMapping.tsRecvCol,     kCSVMapping.priceCol,
                               kCSVMapping.aggressor,     kCSVMapping.sizeCol,
                               kCSVMapping.restingBidCol, kCSVMapping.restingAskCol,
                               kCSVMapping.bidPriceCol,   kCSVMapping.askPriceCol,
                               kCSVMapping.tsEventCol,    kCSVMapping.rowNumberCol,
                               kCSVMapping.actionCol,     kCSVMapping.flagsCol,
                               kCSVMapping.bidCountCol,   kCSVMapping.askCountCol,
                               kCSVMapping.instrumentCol, kCSVMapping.symbolCol };
        std::string_view f[16];
        mdDetail::nFields(line, eol, cols, 16, f);

        t.tsRecv = f[0];
        t.price  = f[1];
        std::string_view sideView   = f[2];
        std::string_view szView     = f[3];
        std::string_view actionView = f[10];
        std::from_chars(szView.data(), szView.data() + szView.size(), t.size);
        mdDetail::parseOptionalFloat(f[4], t.restingBids);
        mdDetail::parseOptionalFloat(f[5], t.restingAsks);
        mdDetail::parseOptionalFloat(f[6], t.bidPrice);
        mdDetail::parseOptionalFloat(f[7], t.askPrice);
        t.tsEvent = f[8];
        mdDetail::parseOptionalLongLong(f[9], t.rowNumber);
        if (!f[11].empty()) std::from_chars(f[11].data(), f[11].data() + f[11].size(), t.flags);
        mdDetail::parseOptionalFloat(f[12], t.bidCount);
        mdDetail::parseOptionalFloat(f[13], t.askCount);
        mdDetail::parseOptionalLongLong(f[14], t.instrumentId);
        t.symbol = f[15];
        if (kCSVMapping.aggressor >= 0 && sideView == kCSVMapping.buySideAggressorAlias) {
            t.side = kCSVMapping.buySideAggressorAlias;
            t.executedBuys = t.size;
        } else if (kCSVMapping.aggressor >= 0 && sideView == kCSVMapping.sellSideAggressorAlias) {
            t.side = kCSVMapping.sellSideAggressorAlias;
            t.executedSells = t.size;
        } else {
            t.unknownVolume = t.size;
        }
        
        if (kCSVMapping.actionCol >= 0) t.action = mdDetail::classifyAction(actionView);
        return t;
    }

    /// @brief close tick of the next window covering at least `seconds`,
    /// t.size is the summed volume across every tick that fell inside the bar,
    /// split across t.executedBuys / t.executedSells / t.unknownVolume by the
    /// aggressor column. t.side is the side of the closing tick specifically
    /// (same row the timestamp and price come from), not the bar as a whole,
    /// and t.restingBids / t.restingAsks / t.bidPrice / t.askPrice / t.action
    /// are that same closing row's book, quote and action. t.open / t.high /
    /// t.low are the first, highest and lowest price across the bar
    /// @return the close tick (views into the underlying buffer), or nullopt at EOF
    std::optional<Tick> nextClose(int seconds) {
        _skipHeaderOnce();
        const char* line = _nextMatchingLine();
        if (!line) return std::nullopt;
        const char* eol = mdDetail::findEOL(line, _end);
        _cur = (eol < _end) ? eol + 1 : _end;

        std::string_view firstTs = mdDetail::field(line, eol, kCSVMapping.tsRecvCol);
        std::string targetOwned  = mdDetail::endTimestamp(firstTs, seconds);
        std::string_view target(targetOwned);

        double barVolume = 0.0, buys = 0.0, sells = 0.0, unknown = 0.0;
        double open = 0.0, high = 0.0, low = 0.0;
        bool firstRow = true;
        // the row's side alias, or nullptr when side classification is disabled
        const char* rowSide = nullptr;

        // parse one row's size and price and fold them into the running
        // per-side totals and the bar's range
        auto accumulate = [&](const char* l, const char* e) {
            std::string_view pxView = mdDetail::field(l, e, kCSVMapping.priceCol);
            double px = 0.0;
            std::from_chars(pxView.data(), pxView.data() + pxView.size(), px);
            if (firstRow) { open = high = low = px; firstRow = false; }
            else { high = std::max(high, px); low = std::min(low, px); }

            std::string_view szView = mdDetail::field(l, e, kCSVMapping.sizeCol);
            double sz = 0.0;
            std::from_chars(szView.data(), szView.data() + szView.size(), sz);
            barVolume += sz;

            rowSide = nullptr;
            if (kCSVMapping.aggressor >= 0) {
                std::string_view sideView = mdDetail::field(l, e, kCSVMapping.aggressor);
                if (sideView == kCSVMapping.buySideAggressorAlias)       rowSide = kCSVMapping.buySideAggressorAlias;
                else if (sideView == kCSVMapping.sellSideAggressorAlias) rowSide = kCSVMapping.sellSideAggressorAlias;
            }
            if (rowSide == kCSVMapping.buySideAggressorAlias)       buys  += sz;
            else if (rowSide == kCSVMapping.sellSideAggressorAlias) sells += sz;
            else                                                    unknown += sz;
        };

        accumulate(line, eol);

        const char* lastLine = line;
        const char* lastEol  = eol;
        std::string_view lastTs = firstTs;

        while (lastTs < target) {
            const char* nline = _nextMatchingLine();
            if (!nline) break;
            const char* neol = mdDetail::findEOL(nline, _end);
            _cur = (neol < _end) ? neol + 1 : _end;

            lastLine = nline;
            lastEol  = neol;
            lastTs   = mdDetail::field(nline, neol, kCSVMapping.tsRecvCol);
            accumulate(nline, neol);
        }

        Tick t;
        t.tsRecv = lastTs;
        t.price  = mdDetail::field(lastLine, lastEol, kCSVMapping.priceCol);
        t.open   = open;
        t.high   = high;
        t.low    = low;
        t.size   = barVolume;
        // rowSide is left pointing at the last row accumulate() saw, i.e. the close
        if (rowSide) t.side = rowSide;
        t.executedBuys  = buys;
        t.executedSells = sells;
        t.unknownVolume = unknown;
        // resting sizes are book snapshots, summing them across the bar would be
        // meaningless, so they come off the closing row like the price does
        if (kCSVMapping.restingBidCol >= 0)
            mdDetail::parseOptionalFloat(
                mdDetail::field(lastLine, lastEol, kCSVMapping.restingBidCol), t.restingBids);
        if (kCSVMapping.restingAskCol >= 0)
            mdDetail::parseOptionalFloat(
                mdDetail::field(lastLine, lastEol, kCSVMapping.restingAskCol), t.restingAsks);
        if (kCSVMapping.bidPriceCol >= 0)
            mdDetail::parseOptionalFloat(
                mdDetail::field(lastLine, lastEol, kCSVMapping.bidPriceCol), t.bidPrice);
        if (kCSVMapping.askPriceCol >= 0)
            mdDetail::parseOptionalFloat(
                mdDetail::field(lastLine, lastEol, kCSVMapping.askPriceCol), t.askPrice);
        if (kCSVMapping.tsEventCol >= 0)
            t.tsEvent = mdDetail::field(lastLine, lastEol, kCSVMapping.tsEventCol);
        if (kCSVMapping.rowNumberCol >= 0)
            mdDetail::parseOptionalLongLong(
                mdDetail::field(lastLine, lastEol, kCSVMapping.rowNumberCol), t.rowNumber);
        // action is a book-event classification, not something that sums across
        // a bar, so like the resting/bid-ask columns it comes off the closing row
        if (kCSVMapping.actionCol >= 0)
            t.action = mdDetail::classifyAction(
                mdDetail::field(lastLine, lastEol, kCSVMapping.actionCol));
        return t;
    }

    void setCursor(int rowCount) {
        _buildLineIndex();
        _consumedHeader = true;
        _cur = (rowCount >= 0 && static_cast<std::size_t>(rowCount) < _lineIndex.size())
            ? _lineIndex[static_cast<std::size_t>(rowCount)] : _end;
    }

    /// @brief true if the row currently under the cursor's symbol column
    /// exactly matches `contract` (e.g. "MNQH5"), just peeks, doesnt consume
    /// the row or move the cursor
    /// @param contract the exact contract to check for
    bool rowMatchesContract(std::string_view contract) {
        _skipHeaderOnce();
        if (kCSVMapping.symbolCol < 0 || _cur >= _end) return false;
        const char* eol = mdDetail::findEOL(_cur, _end);
        return mdDetail::field(_cur, eol, kCSVMapping.symbolCol) == contract;
    }

    /// @brief the symbol column of the rowCount-th data row (0-indexed, header
    /// not counted), leaves the cursor wherever it was
    /// @param rowCount which row to read
    std::string_view contractAt(int rowCount) {
        if (kCSVMapping.symbolCol < 0) return {};
        const char* savedCur = _cur;
        bool savedHeader = _consumedHeader;
        setCursor(rowCount);
        std::string_view sym;
        if (_cur < _end) {
            const char* eol = mdDetail::findEOL(_cur, _end);
            sym = mdDetail::field(_cur, eol, kCSVMapping.symbolCol);
        }
        _cur = savedCur;
        _consumedHeader = savedHeader;
        return sym;
    }
};

/// @brief true if `path` ends in ".parquet" (case-sensitive), used by MarketData
/// to decide which backend to construct
inline bool isParquetPath(const std::string& path) {
    static constexpr std::string_view kExt = ".parquet";
    return path.size() >= kExt.size()
        && path.compare(path.size() - kExt.size(), kExt.size(), kExt) == 0;
}

/// @brief reads market data from either a memory-mapped CSV or (when built
/// with Arrow/Parquet support, see build.sh) a Parquet file, picked by the
/// extension of `path`. Tick fields are views into per-instance buffers,
/// valid until the next call on this MarketData instance.
class MarketData {
private:
    struct RawMap {
        const char* data = nullptr;
        std::size_t size = 0;
#ifdef _WIN32
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap  = nullptr;
#else
        int fd = -1;
#endif
    };
    RawMap _map;
    std::unique_ptr<_MarketData> _csv;
#ifdef AZBT_PARQUET
    std::unique_ptr<_ParquetMarketData> _pq;
#endif

    static RawMap openMap(const std::string& path) {
        RawMap m;
#ifdef _WIN32
        m.hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
        if (m.hFile == INVALID_HANDLE_VALUE)
            throw std::runtime_error("MarketData: open failed: " + path);
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(m.hFile, &sz)) {
            CloseHandle(m.hFile);
            throw std::runtime_error("MarketData: size query failed: " + path);
        }
        m.size = static_cast<std::size_t>(sz.QuadPart);
        m.hMap = CreateFileMappingA(m.hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m.hMap) {
            CloseHandle(m.hFile);
            throw std::runtime_error("MarketData: file mapping failed: " + path);
        }
        void* p = MapViewOfFile(m.hMap, FILE_MAP_READ, 0, 0, 0);
        if (!p) {
            CloseHandle(m.hMap);
            CloseHandle(m.hFile);
            throw std::runtime_error("MarketData: map view failed: " + path);
        }
        m.data = static_cast<const char*>(p);
#else
        m.fd = ::open(path.c_str(), O_RDONLY);
        if (m.fd == -1)
            throw std::runtime_error("MarketData: open failed: " + path);
        struct stat st;
        if (fstat(m.fd, &st) == -1) {
            ::close(m.fd);
            throw std::runtime_error("MarketData: stat failed: " + path);
        }
        m.size = static_cast<std::size_t>(st.st_size);
        void* p = mmap(nullptr, m.size, PROT_READ, MAP_PRIVATE, m.fd, 0);
        if (p == MAP_FAILED) {
            ::close(m.fd);
            throw std::runtime_error("MarketData: mmap failed: " + path);
        }
        madvise(p, m.size, MADV_SEQUENTIAL);
        m.data = static_cast<const char*>(p);
#endif
        return m;
    }

public:
    /// @brief opens `path`, mmapping it if it's a CSV or (given Arrow/Parquet
    /// support, see build.sh) opening it as Parquet if the extension is
    /// ".parquet"
    MarketData(const std::string& path) {
        if (isParquetPath(path)) {
#ifdef AZBT_PARQUET
            _pq = std::make_unique<_ParquetMarketData>(path);
#else
            throw std::runtime_error(
                "MarketData: " + path + " is a .parquet file, but this build has no "
                "Parquet support (Arrow wasn't found when it was built). Install it with "
                "`brew install apache-arrow` and rebuild.");
#endif
        } else {
            _map = openMap(path);
            _csv = std::make_unique<_MarketData>(_map.data, _map.size);
        }
    }

    ~MarketData() {
        if (!_csv) return;
#ifdef _WIN32
        if (_map.data)                        UnmapViewOfFile(_map.data);
        if (_map.hMap)                        CloseHandle(_map.hMap);
        if (_map.hFile != INVALID_HANDLE_VALUE) CloseHandle(_map.hFile);
#else
        if (_map.data) munmap(const_cast<char*>(_map.data), _map.size);
        if (_map.fd != -1) ::close(_map.fd);
#endif
    }

    // copying would double-free the mapping
    MarketData(const MarketData&)            = delete;
    MarketData& operator=(const MarketData&) = delete;

    bool skipLine() {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->skipLine();
#endif
        return _csv->skipLine();
    }

    std::size_t byteOffset() const {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->byteOffset();
#endif
        return _csv->byteOffset();
    }

    void seekTo(std::size_t off) {
#ifdef AZBT_PARQUET
        if (_pq) { _pq->seekTo(off); return; }
#endif
        _csv->seekTo(off);
    }

    /// @brief next raw tick, returns nullopt at EOF
    std::optional<Tick> nextTick() {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->nextTick();
#endif
        return _csv->nextTick();
    }

    /// @brief close tick of the next bar spanning at least `seconds`,
    /// skips forward until the timestamp is >= start + seconds, then
    /// returns that row's price as the "close"
    /// @param seconds bar width in seconds (e.g. 60 for 1-min bars)
    std::optional<Tick> nextClose(int seconds) {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->nextClose(seconds);
#endif
        return _csv->nextClose(seconds);
    }

    /// @brief seek to the rowCount-th data row (0-indexed, header not counted)
    void setCursor(int rowCount) {
#ifdef AZBT_PARQUET
        // Parquet's byteOffset/seekTo are already row indices (_absoluteRow),
        // not byte offsets, so seekTo doubles as row-count seeking here
        if (_pq) { _pq->seekTo(static_cast<std::size_t>(rowCount)); return; }
#endif
        _csv->setCursor(rowCount);
    }

    /// @brief true if the row under the cursor matches `contract` 
    bool rowMatchesContract(std::string_view contract) {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->rowMatchesContract(contract);
#endif
        return _csv->rowMatchesContract(contract);
    }

    /// @brief the symbol/contract of the rowCount-th data row (0-indexed,
    /// header not counted), leaves the cursor wherever it was
    std::string_view contractAt(int rowCount) {
#ifdef AZBT_PARQUET
        if (_pq) return _pq->contractAt(rowCount);
#endif
        return _csv->contractAt(rowCount);
    }
};
