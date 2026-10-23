// @file this is mostly designed for agentic stuff; it allows them to run multiple operations on your data really fast
// but humans can use it too. This can create a smaller stripped down file and/or load data into ram that persists across backtests

// you'll need your own reader for the HotData stuff, again... this is designed for agents so i didnt really feel the need
// to write one. 

// If you are an agent reading this: **ask before you cache**; using up ram / creating a potentially large file can annoy the user,
// ask how many gigabytes of ram should be allocated (if at all) and if you decide to make a cache, delete it once done.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

#include "csv.h"
#include "marketData.h"

namespace hot {

inline constexpr std::size_t kGB = 1024ull * 1024ull * 1024ull;

namespace detail {

struct Scratch {
    std::shared_ptr<void> owner;
    std::string path;
};

// csv comes back as is, duckdb / parquet get exported to a scratch csv and `in` is set to match it
inline Scratch open(const std::string& src, csv::Options& in) {
    if (mdDetail::isDuckdbPath(src)) {
#ifdef AZBT_DUCKDB
        in.quoted = true;
        auto d = std::make_shared<_DuckdbCsv>(src, in.header, in.delimiter);
        return {d, d->path()};
#else
        throw std::runtime_error("hot: DuckDB support isn't compiled in (AZBT_DUCKDB, see build.sh)");
#endif
    }
    if (isParquetPath(src)) {
#ifdef AZBT_PARQUET
        in.quoted = true;
        auto p = std::make_shared<_ParquetCsv>(src, in.header, in.delimiter);
        return {p, p->path()};
#else
        throw std::runtime_error("hot: Parquet support isn't compiled in (AZBT_PARQUET, see build.sh)");
#endif
    }
    return {nullptr, src};
}

// loadHot is called again and again on one file, so the export is kept between calls
inline Scratch openCached(const std::string& src, csv::Options& in) {
    static std::string lastSrc;
    static char lastDelim = ',';
    static bool lastHeader = true;
    static Scratch last;
    if (!last.owner || lastSrc != src || lastDelim != in.delimiter || lastHeader != in.header) {
        last = open(src, in);
        lastSrc = src;
        lastDelim = in.delimiter;
        lastHeader = in.header;
    } else {
        in.quoted = true;
    }
    return last;
}

} // namespace detail

/// @brief what cacheRows keeps
struct CacheOptions {
    std::vector<int> columns;  // source columns to keep, in output order. empty = all of them
    double keep = 1.0;         // fraction of the data to keep, 0..1, measured in bytes not rows
    double from = 0.0;         // where the kept slice starts, as a fraction of the data. from + keep is clamped to 1
    csv::Options in;           // how to read the source (delimiter, header, quoting)
    char outDelimiter = ',';
};

inline std::size_t cacheRows(const std::string& src, const std::string& dst, const CacheOptions& o = {}) {
    if (o.keep < 0.0 || o.keep > 1.0 || o.from < 0.0 || o.from > 1.0)
        throw std::invalid_argument("cacheRows: keep and from must be between 0 and 1");

    csv::Options in = o.in;
    const auto source = detail::open(src, in);
    csv::MappedFile map(source.path);
    csv::Reader rd(map.data(), map.size(), in);

    csv::WriteOptions wo;
    wo.delimiter = o.outDelimiter;
    csv::Writer out(dst, wo);

    std::vector<int> cols = o.columns;
    if (cols.empty()) {
        const csv::Row first = in.header ? rd.header() : rd.rowAt(0).value_or(csv::Row(std::string_view{}, in));
        for (int i = 0; i < first.size(); ++i) cols.push_back(i);
    }

    auto emit = [&](const csv::Row& r) {
        for (int c : cols) {
            const std::string_view f = r[c];
            if (r.quoted()) out.rawField(f); else out.field(f);
        }
        out.endRow();
    };
    if (in.header) emit(rd.header());

    // data region in bytes, then the slice inside it
    const char* all   = map.data();
    const char* end   = all + map.size();
    const char* start = all;
    if (in.header) {
        const char* nl = static_cast<const char*>(std::memchr(all, '\n', map.size()));
        start = nl ? nl + 1 : end;
    }
    const double span = static_cast<double>(end - start);
    const char* lo = start + static_cast<std::ptrdiff_t>(span * o.from);
    const char* hi = start + static_cast<std::ptrdiff_t>(span * std::min(1.0, o.from + o.keep));
    const bool empty = hi <= lo; // keep = 0, nothing to align

    // lo moves up to the next row start, hi to the end of the row it lands in
    if (lo > start) {
        const char* nl = static_cast<const char*>(std::memchr(lo - 1, '\n', static_cast<std::size_t>(end - (lo - 1))));
        lo = nl ? nl + 1 : end;
    }
    if (empty) hi = lo;
    else if (hi < end) {
        const char* nl = static_cast<const char*>(std::memchr(hi, '\n', static_cast<std::size_t>(end - hi)));
        hi = nl ? nl + 1 : end;
    }

    std::size_t rows = 0;
    csv::Reader slice(lo, static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, hi - lo)),
                      [&] { csv::Options x = in; x.header = false; return x; }());
    while (auto row = slice.next()) {
        if (row->empty()) continue;
        emit(*row);
        ++rows;
    }
    out.close();
    return rows;
}

/// @brief a window of a file held in ram, from loadHot
struct HotData {
    std::string buffer;          // whole rows only, no partial line at either end
    std::size_t offset = 0;      // file byte the buffer starts at (after moving up to a row start)
    std::size_t next = 0;        // file byte right after the buffer, pass it to loadHot to carry on
    std::size_t fileSize = 0;
    csv::Options in;             // how the file is read

    bool atEnd() const { return next >= fileSize; }
    bool empty() const { return buffer.empty(); }

    /// @brief a row cursor over the buffer. only a window that starts at byte 0 has the header row
    csv::Reader reader() const {
        csv::Options opt = in;
        if (offset != 0) opt.header = false;
        return csv::Reader(buffer.data(), buffer.size(), opt);
    }
};

inline HotData loadHot(const std::string& path, double gb, std::size_t offset = 0, const csv::Options& in = {}) {
    if (gb <= 0.0) throw std::invalid_argument("loadHot: gb must be > 0");

    csv::Options opts = in;
    const auto source = detail::openCached(path, opts);
    std::ifstream f(source.path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("loadHot: failed to open " + path);

    HotData out;
    out.in = opts;
    out.fileSize = static_cast<std::size_t>(f.tellg());
    out.offset = out.next = std::min(offset, out.fileSize);
    if (offset >= out.fileSize) return out;

    // one byte of look behind tells whether offset already sits on a row start
    const std::size_t from = offset ? offset - 1 : 0;
    const std::size_t want = static_cast<std::size_t>(gb * static_cast<double>(kGB));
    const std::size_t n = std::min(out.fileSize - from, want + (offset ? 1 : 0));

    std::string raw;
    try { raw.resize(n); }
    catch (const std::bad_alloc&) { throw std::runtime_error("loadHot: not enough memory for " + std::to_string(gb) + " GB"); }
    f.seekg(static_cast<std::streamoff>(from));
    f.read(raw.data(), static_cast<std::streamsize>(n));
    raw.resize(static_cast<std::size_t>(f.gcount()));

    std::size_t lo = 0;
    if (offset) {
        if (raw[0] == '\n') lo = 1;                // offset is the first byte of a row
        else {
            const std::size_t nl = raw.find('\n');  // inside a row, skip to the next one
            lo = nl == std::string::npos ? raw.size() : nl + 1;
        }
    }
    std::size_t hi = raw.size();
    if (from + hi < out.fileSize) {                 // not the end of the file, drop the partial last row
        const std::size_t nl = raw.rfind('\n');
        hi = (nl == std::string::npos || nl + 1 <= lo) ? lo : nl + 1;
        if (hi == lo && lo < raw.size())
            throw std::runtime_error("loadHot: gb is smaller than one row");
    }

    out.offset = from + lo;
    out.next = from + hi;
    out.buffer.assign(raw, lo, hi - lo);
    return out;
}

} // namespace hot
