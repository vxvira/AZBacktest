// I genuinely have no clue how to do this file stuff
// so claude did (most) of it. Methods added recently are likely human written
// writing this as of september 14th 2026, for the record
//
// market data wrapper over the generic csv toolkit in csv.h, this is where
// kCSVMapping turns rows into Ticks (MarketData) and Ticks back into rows
// (MarketDataWriter)

#pragma once

#include "dataConfig.h"
#include "csv.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/// @brief csv options for market data files, the header flag comes from
/// kCSVMapping.skipHeader so a headerless file reads its first row as data
inline csv::Options marketDataCsvOptions() {
    csv::Options opt;
    opt.header = kCSVMapping.skipHeader;
    return opt;
}

/// @brief kCSVMapping-aware reader over a raw byte range, used internally by MarketData
class _MarketData {
private:
    csv::Reader _in;

    // contract roll state
    std::string _activeSymbol;
    int _missCount = 0;
    static constexpr int _rollThreshold = 200;

    // next row that passes the configured symbol filter, consumed
    // handles both exact match and rolling contract mode
    std::optional<csv::Row> _nextMatchingRow() {
        // an empty symbol means "read the column but don't filter on it"
        if (kCSVMapping.symbolCol < 0 || kCSVMapping.symbol[0] == '\0') return _in.next();
        std::string_view root(kCSVMapping.symbol);
        while (auto row = _in.next()) {
            std::string_view sym = (*row)[kCSVMapping.symbolCol];

            if (!kCSVMapping.symbolRoll) {
                // exact match mode
                if (sym == root) return row;
                continue;
            }

            // rolling mode: match root prefix
            if (sym.size() < root.size() || sym.substr(0, root.size()) != root) continue;

            // first contract we see becomes active
            if (_activeSymbol.empty()) {
                _activeSymbol = std::string(sym);
                _missCount = 0;
                return row;
            }

            // matches the active contract
            if (sym == std::string_view(_activeSymbol)) {
                _missCount = 0;
                return row;
            }

            // different contract, count misses and roll if the active one is gone
            if (++_missCount >= _rollThreshold) {
                _activeSymbol = std::string(sym);
                _missCount = 0;
                return row;
            }
        }
        return std::nullopt;
    }

    // one pass over every mapped column of `row`, the optional ones (aggressor,
    // resting sizes, bid/ask prices, ts_event, row number, MBP extras) stay at
    // Tick's defaults when their index is -1. nextTick hands this straight back,
    // nextClose uses it for the closing row's snapshot then swaps in the bar
    // aggregates
    static Tick _parseRow(const csv::Row& row) {
        const int cols[16] = { kCSVMapping.tsRecvCol,     kCSVMapping.priceCol,
                               kCSVMapping.aggressor,     kCSVMapping.sizeCol,
                               kCSVMapping.restingBidCol, kCSVMapping.restingAskCol,
                               kCSVMapping.bidPriceCol,   kCSVMapping.askPriceCol,
                               kCSVMapping.tsEventCol,    kCSVMapping.rowNumberCol,
                               kCSVMapping.actionCol,     kCSVMapping.flagsCol,
                               kCSVMapping.bidCountCol,   kCSVMapping.askCountCol,
                               kCSVMapping.instrumentCol, kCSVMapping.symbolCol };
        std::string_view f[16];
        row.fields(cols, 16, f);

        Tick t;
        t.tsRecv = f[0];
        t.price  = f[1];
        csv::parse(f[3],  t.size);
        csv::parse(f[4],  t.restingBids);
        csv::parse(f[5],  t.restingAsks);
        csv::parse(f[6],  t.bidPrice);
        csv::parse(f[7],  t.askPrice);
        t.tsEvent = f[8];
        csv::parse(f[9],  t.rowNumber);
        csv::parse(f[11], t.flags);
        csv::parse(f[12], t.bidCount);
        csv::parse(f[13], t.askCount);
        csv::parse(f[14], t.instrumentId);
        t.symbol = f[15];

        std::string_view sideView = f[2];
        if (kCSVMapping.aggressor >= 0 && sideView == kCSVMapping.buySideAggressorAlias) {
            t.side = kCSVMapping.buySideAggressorAlias;
            t.executedBuys = t.size;
        } else if (kCSVMapping.aggressor >= 0 && sideView == kCSVMapping.sellSideAggressorAlias) {
            t.side = kCSVMapping.sellSideAggressorAlias;
            t.executedSells = t.size;
        } else {
            t.unknownVolume = t.size;
        }

        if (kCSVMapping.actionCol >= 0) t.action = mdDetail::classifyAction(f[10]);
        return t;
    }

public:
    _MarketData(const char* data, std::size_t size)
        : _in(data, size, marketDataCsvOptions()) {}

    // skip past one line without parsing, returns false at EOF
    bool skipLine() { return _in.skip(); }

    std::size_t byteOffset() const { return _in.byteOffset(); }
    void seekTo(std::size_t off) { _in.seekTo(off); }

    /// @brief next tick from the file (header skipped on first call)
    /// @return the next tick as views into the underlying buffer, or nullopt at EOF
    std::optional<Tick> nextTick() {
        auto row = _nextMatchingRow();
        if (!row) return std::nullopt;
        return _parseRow(*row);
    }

    /// @brief close tick of the next window covering at least `seconds`,
    /// t.size is the summed volume across every tick that fell inside the bar,
    /// split across t.executedBuys / t.executedSells / t.unknownVolume by the
    /// aggressor column. everything else that describes a single row (side,
    /// resting sizes, quote, action, ts_event, MBP extras, symbol) comes off
    /// the closing row, same row the timestamp and price come from. t.open /
    /// t.high / t.low are the first, highest and lowest price across the bar
    /// @return the close tick (views into the underlying buffer), or nullopt at EOF
    std::optional<Tick> nextClose(int seconds) {
        auto first = _nextMatchingRow();
        if (!first) return std::nullopt;

        // the four columns every row in the bar needs, pulled in one pass
        const int cols[4] = { kCSVMapping.tsRecvCol, kCSVMapping.priceCol,
                              kCSVMapping.sizeCol,   kCSVMapping.aggressor };

        double barVolume = 0.0, buys = 0.0, sells = 0.0, unknown = 0.0;
        double open = 0.0, high = 0.0, low = 0.0;
        bool firstRow = true;

        // fold one row's price and size into the bar's range and the running
        // per-side totals, hands back the row's timestamp
        auto accumulate = [&](const csv::Row& row) {
            std::string_view f[4];
            row.fields(cols, 4, f);

            double px = csv::as<double>(f[1]);
            if (firstRow) { open = high = low = px; firstRow = false; }
            else { high = std::max(high, px); low = std::min(low, px); }

            double sz = csv::as<double>(f[2]);
            barVolume += sz;
            if (kCSVMapping.aggressor >= 0 && f[3] == kCSVMapping.buySideAggressorAlias)       buys    += sz;
            else if (kCSVMapping.aggressor >= 0 && f[3] == kCSVMapping.sellSideAggressorAlias) sells   += sz;
            else                                                                               unknown += sz;
            return f[0];
        };

        std::string_view firstTs = accumulate(*first);
        std::string targetOwned  = mdDetail::endTimestamp(firstTs, seconds);
        std::string_view target(targetOwned);

        csv::Row last = *first;
        std::string_view lastTs = firstTs;
        while (lastTs < target) {
            auto row = _nextMatchingRow();
            if (!row) break;
            last   = *row;
            lastTs = accumulate(last);
        }

        // book, quote and action are snapshots, summing them across the bar
        // would be meaningless, so they come off the closing row like the price
        Tick t = _parseRow(last);
        t.open = open;
        t.high = high;
        t.low  = low;
        t.size = barVolume;
        t.executedBuys  = buys;
        t.executedSells = sells;
        t.unknownVolume = unknown;
        return t;
    }

    void setCursor(int rowCount) { _in.seekRow(rowCount); }

    /// @brief true if the row currently under the cursor's symbol column
    /// exactly matches `contract` (e.g. "MNQH5"), just peeks, doesnt consume
    /// the row or move the cursor
    /// @param contract the exact contract to check for
    bool rowMatchesContract(std::string_view contract) {
        if (kCSVMapping.symbolCol < 0) return false;
        auto row = _in.peek();
        return row && (*row)[kCSVMapping.symbolCol] == contract;
    }

    /// @brief the symbol column of the rowCount-th data row (0-indexed, header
    /// not counted), leaves the cursor wherever it was
    /// @param rowCount which row to read
    std::string_view contractAt(int rowCount) {
        if (kCSVMapping.symbolCol < 0) return {};
        auto row = _in.rowAt(rowCount);
        return row ? (*row)[kCSVMapping.symbolCol] : std::string_view{};
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
    csv::MappedFile _map; // declared before _csv so it outlives the reader pointing into it
    std::unique_ptr<_MarketData> _csv;
#ifdef AZBT_PARQUET
    std::unique_ptr<_ParquetMarketData> _pq;
#endif

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
            _map = csv::MappedFile(path);
            _csv = std::make_unique<_MarketData>(_map.data(), _map.size());
        }
    }

    // the reader points into _map, moving or copying would leave it dangling
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

/// @brief writes Ticks out as CSV in the kCSVMapping layout, so the file reads
/// straight back through MarketData with the same config. handy for saving a
/// filtered or resampled slice of a big file, e.g. dumping 1 minute bars:
///
///   MarketData md(kCSVMapping.path);
///   MarketDataWriter out("bars_1m.csv");
///   while (auto bar = md.nextClose(60)) out.write(*bar);
///
/// every mapped column gets written at its configured index, unmapped columns
/// in between are left blank. a header row goes out first when
/// kCSVMapping.skipHeader is on (and the file isn't being appended to), since
/// the reader would eat the first data row otherwise
///
/// bars from nextClose write their summed size and closing row snapshot,
/// open/high/low and the buy/sell split have no column in the mapping so they
/// dont survive the trip. a tick with no symbol (Parquet doesn't fill it) gets
/// kCSVMapping.symbol so the symbol filter still matches on the way back in
class MarketDataWriter {
private:
    enum class Slot : unsigned char {
        None, TsRecv, TsEvent, Price, Size, Side, RestingBid, RestingAsk,
        BidPrice, AskPrice, RowNumber, Action, Flags, BidCount, AskCount,
        Instrument, Symbol
    };

    csv::Writer _out;
    std::vector<Slot> _layout; // what goes in each column, index = column
    bool _wroteHeader = false;

    static std::vector<Slot> _buildLayout() {
        const std::pair<int, Slot> mapped[] = {
            {kCSVMapping.tsRecvCol,     Slot::TsRecv},
            {kCSVMapping.tsEventCol,    Slot::TsEvent},
            {kCSVMapping.priceCol,      Slot::Price},
            {kCSVMapping.sizeCol,       Slot::Size},
            {kCSVMapping.aggressor,     Slot::Side},
            {kCSVMapping.restingBidCol, Slot::RestingBid},
            {kCSVMapping.restingAskCol, Slot::RestingAsk},
            {kCSVMapping.bidPriceCol,   Slot::BidPrice},
            {kCSVMapping.askPriceCol,   Slot::AskPrice},
            {kCSVMapping.rowNumberCol,  Slot::RowNumber},
            {kCSVMapping.actionCol,     Slot::Action},
            {kCSVMapping.flagsCol,      Slot::Flags},
            {kCSVMapping.bidCountCol,   Slot::BidCount},
            {kCSVMapping.askCountCol,   Slot::AskCount},
            {kCSVMapping.instrumentCol, Slot::Instrument},
            {kCSVMapping.symbolCol,     Slot::Symbol},
        };
        int width = 0;
        for (const auto& m : mapped) width = std::max(width, m.first + 1);
        std::vector<Slot> layout(static_cast<std::size_t>(width), Slot::None);
        for (const auto& m : mapped)
            if (m.first >= 0) layout[static_cast<std::size_t>(m.first)] = m.second;
        return layout;
    }

    // Databento's names where there is one
    static const char* _name(Slot s) {
        switch (s) {
            case Slot::TsRecv:     return "ts_recv";
            case Slot::TsEvent:    return "ts_event";
            case Slot::Price:      return "price";
            case Slot::Size:       return "size";
            case Slot::Side:       return "side";
            case Slot::RestingBid: return "bid_sz_00";
            case Slot::RestingAsk: return "ask_sz_00";
            case Slot::BidPrice:   return "bid_px_00";
            case Slot::AskPrice:   return "ask_px_00";
            case Slot::RowNumber:  return "sequence";
            case Slot::Action:     return "action";
            case Slot::Flags:      return "flags";
            case Slot::BidCount:   return "bid_ct_00";
            case Slot::AskCount:   return "ask_ct_00";
            case Slot::Instrument: return "instrument_id";
            case Slot::Symbol:     return "symbol";
            case Slot::None:       break;
        }
        return "";
    }

public:
    /// @param path   where to write
    /// @param append add to the end of an existing file instead of truncating
    ///               it, no header gets written in that case
    explicit MarketDataWriter(const std::string& path, bool append = false)
        : _out(path, [&] { csv::WriteOptions o; o.append = append; return o; }()),
          _layout(_buildLayout()) {
        if (kCSVMapping.skipHeader && !append) {
            for (Slot s : _layout) _out.field(_name(s));
            _out.endRow();
            _wroteHeader = true;
        }
    }

    /// @brief write one tick (or bar) as a row
    void write(const Tick& t) {
        for (Slot s : _layout) {
            switch (s) {
                case Slot::TsRecv:     _out.field(t.tsRecv);       break;
                case Slot::TsEvent:    _out.field(t.tsEvent);      break;
                case Slot::Price:      _out.field(t.price);        break;
                case Slot::Size:       _out.field(t.size);         break;
                case Slot::Side:       _out.field(t.side);         break;
                case Slot::RestingBid: _out.field(t.restingBids);  break;
                case Slot::RestingAsk: _out.field(t.restingAsks);  break;
                case Slot::BidPrice:   _out.field(t.bidPrice);     break;
                case Slot::AskPrice:   _out.field(t.askPrice);     break;
                case Slot::RowNumber:  _out.field(t.rowNumber);    break;
                case Slot::Action:     _out.field(t.action);       break;
                case Slot::Flags:      _out.field(t.flags);        break;
                case Slot::BidCount:   _out.field(t.bidCount);     break;
                case Slot::AskCount:   _out.field(t.askCount);     break;
                case Slot::Instrument: _out.field(t.instrumentId); break;
                case Slot::Symbol:
                    _out.field(t.symbol.empty() ? std::string_view(kCSVMapping.symbol) : t.symbol);
                    break;
                case Slot::None:       _out.field("");             break;
            }
        }
        _out.endRow();
    }

    /// @brief write every tick in [begin, end)
    template <typename It>
    void write(It begin, It end) {
        for (; begin != end; ++begin) write(*begin);
    }

    void flush() { _out.flush(); }
    void close() { _out.close(); }
    /// @brief data rows written so far, header not counted
    std::size_t rowsWritten() const { return _out.rowsWritten() - (_wroteHeader ? 1 : 0); }
};
