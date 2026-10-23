// DuckDB-backed market data source. The exporter (_DuckdbCsv) is compiled in only when build.sh finds
// DuckDB on the system and defines AZBT_DUCKDB, the path helpers are always there so a .duckdb path
// without support throws a readable error from MarketData's constructor (same deal as the Parquet backend)
#pragma once

#include <string>
#include <string_view>

namespace mdDetail {

/// @brief a data path split into the database file and what to read from it. the part after a '#' is
/// either a table name, "ticks.duckdb#mnq_ticks", or any SELECT / WITH query, "ticks.duckdb#SELECT * FROM
/// t WHERE size > 0". empty reads the first table in the file by name
struct DuckdbSource { std::string file; std::string what; };

inline DuckdbSource splitDuckdbPath(const std::string& path) {
    const auto hash = path.find('#');
    if (hash == std::string::npos) return {path, ""};
    return {path.substr(0, hash), path.substr(hash + 1)};
}

/// @brief true for a .duckdb / .ddb file, optionally followed by #table or #query
inline bool isDuckdbPath(const std::string& path) {
    const std::string f = splitDuckdbPath(path).file;
    auto ends = [&](std::string_view ext) { return f.size() >= ext.size() && f.compare(f.size() - ext.size(), ext.size(), ext) == 0; };
    return ends(".duckdb") || ends(".ddb");
}

} // namespace mdDetail

#ifdef AZBT_DUCKDB

#include "dataConfig.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include <duckdb.h>

/// @brief a DuckDB table or query exported to a scratch CSV in the layout the CSV reader expects (a
/// header row of column names when `header`, then one line per row in table order, timestamps written
/// ISO-8601 so every time format keeps working). DuckDB streams it to disk, so the data doesn't have to
/// fit in memory, the reader then maps the file like any other CSV. the scratch file is removed when
/// this goes out of scope, so it has to outlive whatever maps it
class _DuckdbCsv {
private:
    std::string _path;

    static std::string _quote(const std::string& s) {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out.push_back('\''); out.push_back(c); }
        out.push_back('\'');
        return out;
    }

    static std::string _ident(const std::string& name) {
        std::string out = "\"";
        for (char c : name) { if (c == '"') out.push_back('"'); out.push_back(c); }
        out.push_back('"');
        return out;
    }

    static std::string _delim(char d) {
        return d == '\t' ? "\t" : std::string(1, d);
    }

public:
    _DuckdbCsv(const std::string& path, bool header, char delimiter = kCSVMapping.delimiter) {
        const auto src = mdDetail::splitDuckdbPath(path);

        duckdb_config cfg = nullptr;
        duckdb_create_config(&cfg);
        duckdb_set_config(cfg, "access_mode", "READ_ONLY");
        duckdb_database db = nullptr;
        char* err = nullptr;
        const duckdb_state opened = duckdb_open_ext(src.file.c_str(), &db, cfg, &err);
        duckdb_destroy_config(&cfg);
        if (opened != DuckDBSuccess) {
            const std::string msg = err ? err : "unknown error";
            duckdb_free(err);
            throw std::runtime_error("MarketData: failed to open " + src.file + " (" + msg + ")");
        }
        duckdb_connection con = nullptr;
        duckdb_connect(db, &con);

        auto run = [&](const std::string& sql, duckdb_result& res) {
            if (duckdb_query(con, sql.c_str(), &res) == DuckDBSuccess) return;
            const std::string msg = duckdb_result_error(&res);
            duckdb_destroy_result(&res);
            duckdb_disconnect(&con);
            duckdb_close(&db);
            throw std::runtime_error("MarketData: " + src.file + ": " + msg);
        };

        std::string what = src.what;
        if (what.empty()) {
            duckdb_result r;
            run("SELECT table_name FROM duckdb_tables() ORDER BY table_name LIMIT 1", r);
            char* name = duckdb_row_count(&r) ? duckdb_value_varchar(&r, 0, 0) : nullptr;
            if (!name) {
                duckdb_destroy_result(&r);
                duckdb_disconnect(&con);
                duckdb_close(&db);
                throw std::runtime_error("MarketData: " + src.file + " has no tables");
            }
            what = name;
            duckdb_free(name);
            duckdb_destroy_result(&r);
        }

        std::string query = what;
        auto starts = [&](const char* kw) {
            std::size_t n = std::char_traits<char>::length(kw);
            if (query.size() < n) return false;
            for (std::size_t i = 0; i < n; ++i) if (std::toupper(static_cast<unsigned char>(query[i])) != kw[i]) return false;
            return true;
        };
        if (!starts("SELECT") && !starts("WITH") && !starts("FROM")) { // a bare table name
            query = "SELECT * FROM " + _ident(what);
        }

        // timestamp columns go out as Databento-style ISO-8601 with nanoseconds, the layout the CSV reader
        // slices, whatever precision the column has. everything else is written as DuckDB prints it
        // (TIMESTAMPTZ needs DuckDB's icu extension, cast it to a plain TIMESTAMP in a query)
        std::string cols;
        {
            duckdb_result d;
            run("DESCRIBE " + query, d);
            for (idx_t i = 0; i < duckdb_row_count(&d); ++i) {
                char* n = duckdb_value_varchar(&d, 0, i);
                char* t = duckdb_value_varchar(&d, 1, i);
                const std::string id = _ident(n ? n : ""), type = t ? t : "";
                duckdb_free(n);
                duckdb_free(t);
                if (!cols.empty()) cols += ", ";
                if (type.rfind("TIMESTAMP", 0) == 0 && type.find("TIME ZONE") == std::string::npos)
                    cols += "strftime(" + id + ", '%Y-%m-%dT%H:%M:%S.') || lpad((epoch_ns(" + id +
                            "::TIMESTAMP_NS) % 1000000000)::VARCHAR, 9, '0') || 'Z' AS " + id;
                else
                    cols += id;
            }
            duckdb_destroy_result(&d);
        }

        static int counter = 0;
        const long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        _path = (std::filesystem::temp_directory_path() /
                 ("azbt_duckdb_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".csv")).generic_string();

        const std::string copy = "COPY (SELECT " + cols + " FROM (" + query + ")) TO " + _quote(_path) +
            " (FORMAT CSV, HEADER " + (header ? "true" : "false") + ", DELIMITER " + _quote(_delim(delimiter)) + ")";
        duckdb_result r;
        try { run(copy, r); } catch (...) { std::remove(_path.c_str()); _path.clear(); throw; }
        duckdb_destroy_result(&r);
        duckdb_disconnect(&con);
        duckdb_close(&db);
    }

    ~_DuckdbCsv() { if (!_path.empty()) std::remove(_path.c_str()); }

    _DuckdbCsv(const _DuckdbCsv&)            = delete;
    _DuckdbCsv& operator=(const _DuckdbCsv&) = delete;

    const std::string& path() const { return _path; }
};

#endif // AZBT_DUCKDB
