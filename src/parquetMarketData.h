// Parquet-backed market data reader. Compiled in only when build.sh detects
// Apache Arrow/Parquet on the system and defines AZBT_PARQUET - otherwise
// this whole file is a no-op, so existing CSV-only builds/collaborators
// don't need Arrow installed at all. See MarketData's constructor in
// marketData.h for the fallback error when a .parquet path is used without
// this support compiled in.
#pragma once

#ifdef AZBT_PARQUET

#include "dataConfig.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

namespace mdDetail {

inline long long tsUnitToNanoMultiplier(arrow::TimeUnit::type unit) {
    switch (unit) {
        case arrow::TimeUnit::SECOND: return 1000000000LL;
        case arrow::TimeUnit::MILLI:  return 1000000LL;
        case arrow::TimeUnit::MICRO:  return 1000LL;
        case arrow::TimeUnit::NANO:   return 1LL;
    }
    return 1LL;
}

} // namespace mdDetail

/// @brief streaming reader over a Parquet file, row-group-at-a-time, mirroring
/// _MarketData's interface (skipLine/byteOffset/seekTo/nextTick/nextClose) so
/// MarketData can dispatch to either backend by file extension.
///
/// The Parquet file is expected to mirror the CSV column layout 1:1 (same
/// column indices as dataConfig.h), just typed instead of all-text.
/// timestamp/price come back through Tick as
/// string_views into a per-instance buffer (formatted on demand from the
/// typed Arrow values), so every existing consumer (tsToEpochSeconds,
/// std::from_chars on price) keeps working unmodified. Those views are only
/// valid until the next call on this instance (narrower than the mmap'd CSV
/// path's whole-lifetime guarantee) - in practice nothing holds a Tick across
/// calls, every call site converts price/timestamp to a numeric type immediately.
class _ParquetMarketData {
private:
    std::unique_ptr<parquet::arrow::FileReader> _reader;
    std::vector<int64_t> _rowGroupOffsets; // cumulative row counts, size == numRowGroups+1
    int64_t _totalRows = 0;

    std::vector<int> _neededCols;          // sorted, deduped parquet column indices to fetch
    int _tsPos = -1, _pxPos = -1, _szPos = -1, _symPos = -1, _aggPos = -1; // position within _neededCols
    int _bidPos = -1, _askPos = -1;        // resting bid/ask size, -1 when not configured
    int _bidPxPos = -1, _askPxPos = -1;    // best bid/ask price, -1 when not configured
    int _tsEventPos = -1;                  // ts_event, -1 when tsEventCol isn't configured
    int _rowNumPos = -1;                   // row/sequence number, -1 when rowNumberCol isn't configured
    int _actionPos = -1;                   // action classification, -1 when not configured
    long long _tsUnitMul = 1;              // multiplier from tsRecvCol's unit to nanoseconds
    long long _tsEventUnitMul = 1;         // multiplier from tsEventCol's unit to nanoseconds

    int _curGroup = -1;
    std::shared_ptr<arrow::Table> _table;  // keeps the current row group's arrays alive
    std::shared_ptr<arrow::TimestampArray> _tsArr;
    std::shared_ptr<arrow::DoubleArray> _pxArr;
    std::shared_ptr<arrow::Int64Array> _szArr;
    std::shared_ptr<arrow::Array> _symArr;
    std::shared_ptr<arrow::Array> _aggArr;
    std::shared_ptr<arrow::Int64Array> _bidArr;
    std::shared_ptr<arrow::Int64Array> _askArr;
    std::shared_ptr<arrow::DoubleArray> _bidPxArr;
    std::shared_ptr<arrow::DoubleArray> _askPxArr;
    std::shared_ptr<arrow::TimestampArray> _tsEventArr;
    std::shared_ptr<arrow::Int64Array> _rowNumArr;
    std::shared_ptr<arrow::Array> _actionArr;
    bool _symLarge = false, _aggLarge = false, _actionLarge = false;
    int64_t _rowInGroup = 0;

    int64_t _absoluteRow = 0; // physical row index across the whole file

    // contract roll state, mirrors _MarketData::_nextMatchingLine
    std::string _activeSymbol;
    int _missCount = 0;
    static constexpr int _rollThreshold = 200;

    char _tsBuf[40];
    char _tsEventBuf[40];
    char _pxBuf[32];

    void loadRowGroup(int g) {
        auto tableResult = _reader->ReadRowGroup(g, _neededCols);
        if (!tableResult.ok())
            throw std::runtime_error("ParquetMarketData: failed to read row group "
                + std::to_string(g) + ": " + tableResult.status().ToString());
        auto combined = (*tableResult)->CombineChunks();
        if (!combined.ok())
            throw std::runtime_error("ParquetMarketData: CombineChunks failed: " + combined.status().ToString());
        _table = *combined;
        _curGroup = g;

        _tsArr  = std::static_pointer_cast<arrow::TimestampArray>(_table->column(_tsPos)->chunk(0));
        _pxArr  = std::static_pointer_cast<arrow::DoubleArray>(_table->column(_pxPos)->chunk(0));
        _szArr  = std::static_pointer_cast<arrow::Int64Array>(_table->column(_szPos)->chunk(0));
        _symArr = _symPos >= 0 ? _table->column(_symPos)->chunk(0) : nullptr;
        _aggArr = _aggPos >= 0 ? _table->column(_aggPos)->chunk(0) : nullptr;
        _bidArr = _bidPos >= 0
            ? std::static_pointer_cast<arrow::Int64Array>(_table->column(_bidPos)->chunk(0)) : nullptr;
        _askArr = _askPos >= 0
            ? std::static_pointer_cast<arrow::Int64Array>(_table->column(_askPos)->chunk(0)) : nullptr;
        _bidPxArr = _bidPxPos >= 0
            ? std::static_pointer_cast<arrow::DoubleArray>(_table->column(_bidPxPos)->chunk(0)) : nullptr;
        _askPxArr = _askPxPos >= 0
            ? std::static_pointer_cast<arrow::DoubleArray>(_table->column(_askPxPos)->chunk(0)) : nullptr;
        _tsEventArr = _tsEventPos >= 0
            ? std::static_pointer_cast<arrow::TimestampArray>(_table->column(_tsEventPos)->chunk(0)) : nullptr;
        _rowNumArr = _rowNumPos >= 0
            ? std::static_pointer_cast<arrow::Int64Array>(_table->column(_rowNumPos)->chunk(0)) : nullptr;
        _actionArr = _actionPos >= 0 ? _table->column(_actionPos)->chunk(0) : nullptr;
    }

    void ensureRowLoaded(int64_t rowIdx) {
        if (_curGroup >= 0 && rowIdx >= _rowGroupOffsets[static_cast<std::size_t>(_curGroup)]
            && rowIdx < _rowGroupOffsets[static_cast<std::size_t>(_curGroup) + 1]) {
            _rowInGroup = rowIdx - _rowGroupOffsets[static_cast<std::size_t>(_curGroup)];
            return;
        }
        int g = static_cast<int>(std::upper_bound(_rowGroupOffsets.begin(), _rowGroupOffsets.end(), rowIdx)
                                  - _rowGroupOffsets.begin()) - 1;
        loadRowGroup(g);
        _rowInGroup = rowIdx - _rowGroupOffsets[static_cast<std::size_t>(_curGroup)];
    }

    std::string_view curSymbol() const {
        return _symLarge ? std::static_pointer_cast<arrow::LargeStringArray>(_symArr)->GetView(_rowInGroup)
                         : std::static_pointer_cast<arrow::StringArray>(_symArr)->GetView(_rowInGroup);
    }
    std::string_view curSide() const {
        return _aggLarge ? std::static_pointer_cast<arrow::LargeStringArray>(_aggArr)->GetView(_rowInGroup)
                         : std::static_pointer_cast<arrow::StringArray>(_aggArr)->GetView(_rowInGroup);
    }
    std::string_view curAction() const {
        return _actionLarge ? std::static_pointer_cast<arrow::LargeStringArray>(_actionArr)->GetView(_rowInGroup)
                            : std::static_pointer_cast<arrow::StringArray>(_actionArr)->GetView(_rowInGroup);
    }
    double  curPrice()   const { return _pxArr->Value(_rowInGroup); }
    int64_t curSizeRaw() const { return _szArr->Value(_rowInGroup); }
    // 0 when the column isn't configured, matching Tick's default and the CSV path
    double curRestingBid() const { return _bidArr ? static_cast<double>(_bidArr->Value(_rowInGroup)) : 0.0; }
    double curRestingAsk() const { return _askArr ? static_cast<double>(_askArr->Value(_rowInGroup)) : 0.0; }
    double curBidPrice()   const { return _bidPxArr ? _bidPxArr->Value(_rowInGroup) : 0.0; }
    double curAskPrice()   const { return _askPxArr ? _askPxArr->Value(_rowInGroup) : 0.0; }
    long long curTsNanos() const { return static_cast<long long>(_tsArr->Value(_rowInGroup)) * _tsUnitMul; }
    bool curHasTsEvent()   const { return _tsEventArr != nullptr; }
    long long curTsEventNanos() const { return static_cast<long long>(_tsEventArr->Value(_rowInGroup)) * _tsEventUnitMul; }
    long long curRowNumber() const { return _rowNumArr ? _rowNumArr->Value(_rowInGroup) : 0; }

    // advance past rows that don't match the configured symbol filter, mirrors
    // _MarketData::_nextMatchingLine but over Parquet rows; leaves _absoluteRow
    // pointing AT the next matching row (not yet consumed) on success
    bool advanceToNextMatch() {
        if (kCSVMapping.symbolCol < 0 || kCSVMapping.symbol[0] == '\0') {
            if (_absoluteRow >= _totalRows) return false;
            ensureRowLoaded(_absoluteRow);
            return true;
        }

        std::string_view root(kCSVMapping.symbol);
        while (_absoluteRow < _totalRows) {
            ensureRowLoaded(_absoluteRow);
            std::string_view sym = curSymbol();

            if (!kCSVMapping.symbolRoll) {
                if (sym == root) return true;
                ++_absoluteRow;
                continue;
            }

            if (sym.size() < root.size() || sym.substr(0, root.size()) != root) {
                ++_absoluteRow;
                continue;
            }

            if (_activeSymbol.empty()) { _activeSymbol = std::string(sym); _missCount = 0; return true; }
            if (sym == std::string_view(_activeSymbol)) { _missCount = 0; return true; }

            _missCount++;
            if (_missCount >= _rollThreshold) { _activeSymbol = std::string(sym); _missCount = 0; return true; }
            ++_absoluteRow;
        }
        return false;
    }

public:
    explicit _ParquetMarketData(const std::string& path) {
        auto fileResult = arrow::io::ReadableFile::Open(path);
        if (!fileResult.ok())
            throw std::runtime_error("ParquetMarketData: open failed: " + path + " (" + fileResult.status().ToString() + ")");

        auto readerResult = parquet::arrow::OpenFile(*fileResult, arrow::default_memory_pool());
        if (!readerResult.ok())
            throw std::runtime_error("ParquetMarketData: failed to open " + path + ": " + readerResult.status().ToString());
        _reader = std::move(*readerResult);

        std::shared_ptr<arrow::Schema> schema;
        auto status = _reader->GetSchema(&schema);
        if (!status.ok())
            throw std::runtime_error("ParquetMarketData: failed to read schema of " + path + ": " + status.ToString());

        auto requireField = [&](int col, const char* role) -> std::shared_ptr<arrow::Field> {
            if (col < 0 || col >= schema->num_fields())
                throw std::runtime_error("ParquetMarketData: configured " + std::string(role) + " column index "
                    + std::to_string(col) + " is out of range for " + path);
            return schema->field(col);
        };

        auto tsField = requireField(kCSVMapping.tsRecvCol, "tsRecv");
        if (tsField->type()->id() != arrow::Type::TIMESTAMP)
            throw std::runtime_error("ParquetMarketData: tsRecvCol is not a timestamp column in " + path
                + " (got " + tsField->type()->ToString() + ")");
        _tsUnitMul = mdDetail::tsUnitToNanoMultiplier(
            std::static_pointer_cast<arrow::TimestampType>(tsField->type())->unit());

        auto pxField = requireField(kCSVMapping.priceCol, "price");
        if (pxField->type()->id() != arrow::Type::DOUBLE)
            throw std::runtime_error("ParquetMarketData: priceCol must be a double column in " + path
                + " (got " + pxField->type()->ToString() + ")");

        auto szField = requireField(kCSVMapping.sizeCol, "size");
        if (szField->type()->id() != arrow::Type::INT64)
            throw std::runtime_error("ParquetMarketData: sizeCol must be an int64 column in " + path
                + " (got " + szField->type()->ToString() + ")");

        if (kCSVMapping.symbolCol >= 0) {
            auto symField = requireField(kCSVMapping.symbolCol, "symbol");
            auto sid = symField->type()->id();
            if (sid != arrow::Type::STRING && sid != arrow::Type::LARGE_STRING)
                throw std::runtime_error("ParquetMarketData: symbolCol must be a string column in " + path
                    + " (got " + symField->type()->ToString() + ")");
            _symLarge = (sid == arrow::Type::LARGE_STRING);
        }
        if (kCSVMapping.aggressor >= 0) {
            auto aggField = requireField(kCSVMapping.aggressor, "aggressor");
            auto aid = aggField->type()->id();
            if (aid != arrow::Type::STRING && aid != arrow::Type::LARGE_STRING)
                throw std::runtime_error("ParquetMarketData: aggressor column must be a string column in " + path
                    + " (got " + aggField->type()->ToString() + ")");
            _aggLarge = (aid == arrow::Type::LARGE_STRING);
        }
        if (kCSVMapping.actionCol >= 0) {
            auto actionField = requireField(kCSVMapping.actionCol, "actionCol");
            auto aid = actionField->type()->id();
            if (aid != arrow::Type::STRING && aid != arrow::Type::LARGE_STRING)
                throw std::runtime_error("ParquetMarketData: actionCol must be a string column in " + path
                    + " (got " + actionField->type()->ToString() + ")");
            _actionLarge = (aid == arrow::Type::LARGE_STRING);
        }

        // resting sizes get the same int64 requirement as sizeCol, they're counts
        auto requireInt64 = [&](int col, const char* role) {
            if (col < 0) return;
            auto f = requireField(col, role);
            if (f->type()->id() != arrow::Type::INT64)
                throw std::runtime_error("ParquetMarketData: " + std::string(role)
                    + " must be an int64 column in " + path + " (got " + f->type()->ToString() + ")");
        };
        requireInt64(kCSVMapping.restingBidCol, "restingBidCol");
        requireInt64(kCSVMapping.restingAskCol, "restingAskCol");

        // bid/ask prices are prices, so same double requirement as priceCol
        auto requireDouble = [&](int col, const char* role) {
            if (col < 0) return;
            auto f = requireField(col, role);
            if (f->type()->id() != arrow::Type::DOUBLE)
                throw std::runtime_error("ParquetMarketData: " + std::string(role)
                    + " must be a double column in " + path + " (got " + f->type()->ToString() + ")");
        };
        requireDouble(kCSVMapping.bidPriceCol, "bidPriceCol");
        requireDouble(kCSVMapping.askPriceCol, "askPriceCol");

        // ts_event is a second timestamp column, same type requirement as tsRecvCol
        if (kCSVMapping.tsEventCol >= 0) {
            auto tsEventField = requireField(kCSVMapping.tsEventCol, "tsEventCol");
            if (tsEventField->type()->id() != arrow::Type::TIMESTAMP)
                throw std::runtime_error("ParquetMarketData: tsEventCol is not a timestamp column in " + path
                    + " (got " + tsEventField->type()->ToString() + ")");
            _tsEventUnitMul = mdDetail::tsUnitToNanoMultiplier(
                std::static_pointer_cast<arrow::TimestampType>(tsEventField->type())->unit());
        }
        // row/sequence number is a plain count, same int64 requirement as the resting sizes
        requireInt64(kCSVMapping.rowNumberCol, "rowNumberCol");

        std::vector<int> cols = { kCSVMapping.tsRecvCol, kCSVMapping.priceCol, kCSVMapping.sizeCol };
        if (kCSVMapping.symbolCol >= 0) cols.push_back(kCSVMapping.symbolCol);
        if (kCSVMapping.aggressor >= 0) cols.push_back(kCSVMapping.aggressor);
        if (kCSVMapping.restingBidCol >= 0) cols.push_back(kCSVMapping.restingBidCol);
        if (kCSVMapping.restingAskCol >= 0) cols.push_back(kCSVMapping.restingAskCol);
        if (kCSVMapping.bidPriceCol >= 0) cols.push_back(kCSVMapping.bidPriceCol);
        if (kCSVMapping.askPriceCol >= 0) cols.push_back(kCSVMapping.askPriceCol);
        if (kCSVMapping.tsEventCol >= 0) cols.push_back(kCSVMapping.tsEventCol);
        if (kCSVMapping.rowNumberCol >= 0) cols.push_back(kCSVMapping.rowNumberCol);
        if (kCSVMapping.actionCol >= 0) cols.push_back(kCSVMapping.actionCol);
        std::sort(cols.begin(), cols.end());
        cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
        _neededCols = cols;

        auto posOf = [&](int col) {
            return static_cast<int>(std::lower_bound(_neededCols.begin(), _neededCols.end(), col) - _neededCols.begin());
        };
        _tsPos  = posOf(kCSVMapping.tsRecvCol);
        _pxPos  = posOf(kCSVMapping.priceCol);
        _szPos  = posOf(kCSVMapping.sizeCol);
        _symPos = kCSVMapping.symbolCol >= 0 ? posOf(kCSVMapping.symbolCol) : -1;
        _aggPos = kCSVMapping.aggressor >= 0 ? posOf(kCSVMapping.aggressor) : -1;
        _bidPos = kCSVMapping.restingBidCol >= 0 ? posOf(kCSVMapping.restingBidCol) : -1;
        _askPos = kCSVMapping.restingAskCol >= 0 ? posOf(kCSVMapping.restingAskCol) : -1;
        _bidPxPos = kCSVMapping.bidPriceCol >= 0 ? posOf(kCSVMapping.bidPriceCol) : -1;
        _askPxPos = kCSVMapping.askPriceCol >= 0 ? posOf(kCSVMapping.askPriceCol) : -1;
        _tsEventPos = kCSVMapping.tsEventCol >= 0 ? posOf(kCSVMapping.tsEventCol) : -1;
        _rowNumPos  = kCSVMapping.rowNumberCol >= 0 ? posOf(kCSVMapping.rowNumberCol) : -1;
        _actionPos  = kCSVMapping.actionCol >= 0 ? posOf(kCSVMapping.actionCol) : -1;

        int numRowGroups = _reader->parquet_reader()->metadata()->num_row_groups();
        _rowGroupOffsets.assign(static_cast<std::size_t>(numRowGroups) + 1, 0);
        for (int i = 0; i < numRowGroups; i++) {
            int64_t rows = _reader->parquet_reader()->metadata()->RowGroup(i)->num_rows();
            _rowGroupOffsets[static_cast<std::size_t>(i) + 1] = _rowGroupOffsets[static_cast<std::size_t>(i)] + rows;
        }
        _totalRows = _rowGroupOffsets.empty() ? 0 : _rowGroupOffsets.back();
    }

    bool skipLine() {
        if (_absoluteRow >= _totalRows) return false;
        ++_absoluteRow;
        return true;
    }

    std::size_t byteOffset() const { return static_cast<std::size_t>(_absoluteRow); }
    void seekTo(std::size_t off)   { _absoluteRow = static_cast<int64_t>(off); }

    /// @brief true if the row under the cursor's symbol column exactly matches
    /// `contract` (e.g. "MNQH5"), just peeks, doesnt advance _absoluteRow
    bool rowMatchesContract(std::string_view contract) {
        if (kCSVMapping.symbolCol < 0 || _absoluteRow >= _totalRows) return false;
        ensureRowLoaded(_absoluteRow);
        return curSymbol() == contract;
    }

    /// @brief the symbol/contract of the rowCount-th row (0-indexed), leaves
    /// _absoluteRow wherever it was
    std::string_view contractAt(int rowCount) {
        if (kCSVMapping.symbolCol < 0 || rowCount < 0 || rowCount >= _totalRows) return {};
        int64_t saved = _absoluteRow;
        ensureRowLoaded(rowCount);
        std::string_view sym = curSymbol();
        _absoluteRow = saved;
        return sym;
    }

    std::optional<Tick> nextTick() {
        if (!advanceToNextMatch()) return std::nullopt;

        Tick t;
        int n = mdDetail::formatIsoTimestamp(_tsBuf, sizeof(_tsBuf), curTsNanos());
        t.tsRecv = std::string_view(_tsBuf, static_cast<std::size_t>(n));

        if (curHasTsEvent()) {
            int en = mdDetail::formatIsoTimestamp(_tsEventBuf, sizeof(_tsEventBuf), curTsEventNanos());
            t.tsEvent = std::string_view(_tsEventBuf, static_cast<std::size_t>(en));
        }
        t.rowNumber = curRowNumber();

        auto [ptr, ec] = std::to_chars(_pxBuf, _pxBuf + sizeof(_pxBuf), curPrice());
        t.price = std::string_view(_pxBuf, static_cast<std::size_t>(ptr - _pxBuf));

        t.size = static_cast<double>(curSizeRaw());

        if (_aggPos >= 0) {
            std::string_view sideView = curSide();
            if (sideView == kCSVMapping.buySideAggressorAlias) t.side = kCSVMapping.buySideAggressorAlias;
            else if (sideView == kCSVMapping.sellSideAggressorAlias) t.side = kCSVMapping.sellSideAggressorAlias;
        }
        if (t.side == kCSVMapping.buySideAggressorAlias)       t.executedBuys  = t.size;
        else if (t.side == kCSVMapping.sellSideAggressorAlias) t.executedSells = t.size;
        else                                                   t.unknownVolume = t.size;

        t.restingBids = curRestingBid();
        t.restingAsks = curRestingAsk();
        t.bidPrice    = curBidPrice();
        t.askPrice    = curAskPrice();

        if (_actionPos >= 0) t.action = mdDetail::classifyAction(curAction());

        ++_absoluteRow;
        return t;
    }

    /// @brief bar close over `seconds`, volume summed and split per aggressor
    /// side, mirrors _MarketData::nextClose. t.side, the resting sizes, the
    /// bid/ask prices, ts_event, row number and action are all the closing row's,
    /// t.open / t.high / t.low are the first, highest and lowest price in the bar
    std::optional<Tick> nextClose(int seconds) {
        if (!advanceToNextMatch()) return std::nullopt;

        int firstLen = mdDetail::formatIsoTimestamp(_tsBuf, sizeof(_tsBuf), curTsNanos());
        std::string firstTs(_tsBuf, static_cast<std::size_t>(firstLen));
        std::string targetOwned = mdDetail::endTimestamp(firstTs, seconds);
        std::string_view target(targetOwned);

        double barVolume = 0.0, buys = 0.0, sells = 0.0, unknown = 0.0;
        // the row's side alias, or nullptr when side classification is disabled
        const char* rowSide = nullptr;

        // fold the row _absoluteRow currently points at into the running totals
        auto accumulate = [&]() {
            double sz = static_cast<double>(curSizeRaw());
            barVolume += sz;

            rowSide = nullptr;
            if (_aggPos >= 0) {
                std::string_view sideView = curSide();
                if (sideView == kCSVMapping.buySideAggressorAlias)       rowSide = kCSVMapping.buySideAggressorAlias;
                else if (sideView == kCSVMapping.sellSideAggressorAlias) rowSide = kCSVMapping.sellSideAggressorAlias;
            }
            if (rowSide == kCSVMapping.buySideAggressorAlias)       buys  += sz;
            else if (rowSide == kCSVMapping.sellSideAggressorAlias) sells += sz;
            else                                                    unknown += sz;
        };

        double lastPx = curPrice();
        double openPx = lastPx, highPx = lastPx, lowPx = lastPx;
        // resting sizes are book snapshots, summing them across the bar would be
        // meaningless, so they track the closing row alongside the price
        double lastBid = curRestingBid(), lastAsk = curRestingAsk();
        double lastBidPx = curBidPrice(), lastAskPx = curAskPrice();
        std::string lastTs = firstTs;
        // ts_event/row number are snapshots too, same closing-row rule
        bool hasTsEvent = curHasTsEvent();
        std::string lastTsEvent = hasTsEvent
            ? std::string(_tsEventBuf, static_cast<std::size_t>(
                  mdDetail::formatIsoTimestamp(_tsEventBuf, sizeof(_tsEventBuf), curTsEventNanos())))
            : std::string();
        long long lastRowNum = curRowNumber();
        // action is a book-event classification, snapshot from the closing row
        // like the resting sizes/bid-ask prices above, not summed across the bar
        const char* lastAction = _actionPos >= 0 ? mdDetail::classifyAction(curAction()) : kCSVMapping.actionNoneAlias;
        accumulate();
        ++_absoluteRow;

        while (std::string_view(lastTs) < target) {
            if (!advanceToNextMatch()) break;
            int len = mdDetail::formatIsoTimestamp(_tsBuf, sizeof(_tsBuf), curTsNanos());
            lastTs.assign(_tsBuf, static_cast<std::size_t>(len));
            lastPx  = curPrice();
            highPx  = std::max(highPx, lastPx);
            lowPx   = std::min(lowPx, lastPx);
            lastBid = curRestingBid();
            lastAsk = curRestingAsk();
            lastBidPx = curBidPrice();
            lastAskPx = curAskPrice();
            if (hasTsEvent) {
                int elen = mdDetail::formatIsoTimestamp(_tsEventBuf, sizeof(_tsEventBuf), curTsEventNanos());
                lastTsEvent.assign(_tsEventBuf, static_cast<std::size_t>(elen));
            }
            lastRowNum = curRowNumber();
            if (_actionPos >= 0) lastAction = mdDetail::classifyAction(curAction());
            accumulate();
            ++_absoluteRow;
        }

        Tick t;
        std::memcpy(_tsBuf, lastTs.data(), lastTs.size());
        t.tsRecv = std::string_view(_tsBuf, lastTs.size());
        if (hasTsEvent) {
            std::memcpy(_tsEventBuf, lastTsEvent.data(), lastTsEvent.size());
            t.tsEvent = std::string_view(_tsEventBuf, lastTsEvent.size());
        }
        t.rowNumber = lastRowNum;
        auto [ptr, ec] = std::to_chars(_pxBuf, _pxBuf + sizeof(_pxBuf), lastPx);
        t.price = std::string_view(_pxBuf, static_cast<std::size_t>(ptr - _pxBuf));
        t.open = openPx;
        t.high = highPx;
        t.low  = lowPx;
        t.size = barVolume;
        // rowSide is left pointing at the last row accumulate() saw, i.e. the close
        if (rowSide) t.side = rowSide;
        t.executedBuys  = buys;
        t.executedSells = sells;
        t.unknownVolume = unknown;
        t.restingBids   = lastBid;
        t.restingAsks   = lastAsk;
        t.bidPrice      = lastBidPx;
        t.askPrice      = lastAskPx;
        t.action        = lastAction;
        return t;
    }
};

#endif // AZBT_PARQUET
