// column mapping and data path, loaded at runtime from config.toml
// call loadConfig() before using kCSVMapping (nothing does it for you)

#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "src/tomlParser.h"

// substring offsets used to pull Y/M/D out of the timestamp column
// defaults match Databento's ISO-8601 UTC nanosecond format:
//   2025-06-01T22:00:00.065308005Z
//    ^^^^ ^^ ^^
//    year mo day
struct DateFormat {
    int yearOffset;
    int yearLength;
    int monthOffset;
    int monthLength;
    int dayOffset;
    int dayLength;
};

/// engine version, bumped with each tagged release
#define AZBT_VERSION "1.2.0"

/// how the timestamp column is written. Iso8601 is Databento's layout and the fast path (bars are
/// windowed by a string compare), everything else is parsed to a number and windowed numerically
enum class TimeFormat { Iso8601, EpochSeconds, EpochMillis, EpochMicros, EpochNanos, Date };

/// how an [extra] column folds into a bar. Last is the closing row's value, same snapshot rule the
/// quote and book columns follow
enum class ExtraAgg { Last, First, Sum, Min, Max, Mean };

/// most [extra] columns a Tick can carry (they live in a fixed array so ticks never allocate)
inline constexpr int kMaxExtras = 16;

/// a user named numeric column carried through to Tick::extras / DataWindow::extras
struct ExtraColumn {
    std::string name;          // what DataWindow::extra("name") looks it up by
    std::string header;        // column name in the file, empty when it was given as an index
    int col = -1;
    ExtraAgg agg = ExtraAgg::Last;
};

enum class FilterOp { Eq, Ne, Lt, Le, Gt, Ge, Contains, NotContains };

/// one [filter] condition, rows that fail it never reach the symbol logic or the bars
struct RowFilter {
    std::string header;        // column name in the file, empty when it was given as an index
    int col = -1;
    FilterOp op = FilterOp::Eq;
    std::string operand;
    double number = 0.0;
    bool numeric = false;      // operand parsed as a number, comparisons then are numeric
};

struct CSVMapping {
    int tsRecvCol;   // required: receive timestamp, also what nextClose windows bars by.
                      // if your data doesn't distinguish recv/event time, just point
                      // this at whatever column carries the timestamp
    int priceCol;
    int sizeCol;

    // absolute path to the data file (CSV or Parquet)
    // use an absolute path so it resolves correctly regardless of
    // which directory the binary runs from
    const char* path;

    DateFormat dateFormat;

    bool skipHeader;

    // event timestamp column, -1 to disable. on Databento TBBO this is ts_event
    // (when the exchange generated the event) as opposed to tsRecvCol/ts_recv
    // (when it was received) - purely carried through to DataWindow, doesn't
    // affect windowing
    int tsEventCol;

    // row/sequence number column, -1 to disable. on Databento TBBO this is
    // `sequence`, a per-instrument monotonic counter from the exchange -
    // carried through to DataWindow.rowNumbers, doesn't affect windowing
    int rowNumberCol;

    // if the CSV has multiple symbols interleaved, set these to filter to one
    int symbolCol;          // column index of the symbol field, -1 to disable
    const char* symbol;     // exact symbol, or root prefix if symbolRoll is true
    bool symbolRoll;        // treat symbol as a prefix and auto-roll when the front contract expires

    // aggressor/side column, read by MarketData::nextTick() to classify each
    // tick's side. defaulted off (-1) so configs that dont set these still work
    int aggressor;                                   // column index of the side field, -1 to disable
    const char* buySideAggressorAlias;               // CSV value meaning "buy aggressor / lifted the ask"
    const char* sellSideAggressorAlias;              // CSV value meaning "sell aggressor / hit the bid"
    const char* unknownSideAggressorAlias;           // fallback when the side column doesnt match either alias

    // resting (top-of-book) size columns, a per-row snapshot of what's sitting
    // on each side of the book rather than volume that actually traded
    // defaulted off (-1) so configs that dont set them still work
    int restingBidCol;                               // column index of the resting bid size, -1 to disable
    int restingAskCol;                               // column index of the resting ask size, -1 to disable

    // best bid/ask price columns, a per-row snapshot of the quote at the time of
    // the trade (on Databento TBBO these are bid_px_00 / ask_px_00). unlike the
    // resting columns these are prices, so Parquet sources need them as doubles
    // defaulted off (-1) so configs that dont set them still work
    int bidPriceCol;                                 // column index of the best bid price, -1 to disable
    int askPriceCol;                                 // column index of the best ask price, -1 to disable


    int actionCol;                                    // column index of the action, -1 to disable 
    const char* actionAddAlias = "A";
    const char* actionCancelAlias = "C";
    const char* actionModifyAlias = "M";
    const char* actionTradeAlias = "T";
    const char* actionFillAlias = "F";
    const char* actionClearAlias = "R";
    const char* actionNoneAlias = "N";

    double commision;                                 // commision, pts
    double spread;                                    // spread, pts
    double timingCost;                                // how much do you lose from latency? (pts)

    // extra MBP columns, all defaulted off (-1). on Databento MBP-1 these are
    // flags(10) (128 = F_LAST, the last record of an exchange packet),
    // bid_ct_00(17) / ask_ct_00(18) (resting order counts at the top level)
    // and instrument_id(4). kept at the end so the positional kCSVMapping
    // initializer below doesn't have to change
    int flagsCol      = -1;
    int bidCountCol   = -1;
    int askCountCol   = -1;
    int instrumentCol = -1;

    // CSV only. when true (and actionCol is mapped) rows whose action isn't actionTradeAlias are skipped
    // before anything sees them. on MBP-1 every add/cancel/modify is a row and its price is a resting
    // order's, so without this bar highs/lows, volume and the closing price are built from book events
    bool tradesOnly = false;

    // ---- generalised data layer (1.2.0), everything below defaults to the old behaviour ----

    TimeFormat timeFormat = TimeFormat::Iso8601;

    char delimiter = ',';
    bool quoted    = false;      // honour "..." around fields, slower reader

    // rows whose price or timestamp is missing / not a number are skipped (and counted) instead of
    // turning into a price 0 bar. CSV only
    bool skipBadRows = true;

    // each row already is a bar (cleaned / pre-aggregated data). priceCol is the close, open / high / low
    // columns are optional and fall back to the close. timeframe is ignored in this mode
    bool rowsAreBars = false;
    int openCol = -1, highCol = -1, lowCol = -1;

    std::vector<ExtraColumn> extras;   // [extra]
    std::vector<RowFilter> filters;    // [filter]

    // *Col keys given as a header name instead of an index land here (config key -> header name) and
    // get resolved against the header when a file is opened, see mdDetail::resolveColumnNames
    std::map<std::string, std::string> colNames;
};

// persistent string storage so const char* fields stay valid
namespace cfgDetail {
    inline std::string pathStr;
    inline std::string symbolStr;
    inline std::string buySideStr    = "B";
    inline std::string sellSideStr   = "S";
    inline std::string unknownSideStr = "N";

    inline std::string actionAddAliasStr = "A";
    inline std::string actionCancelAliasStr = "C";
    inline std::string actionModifyAliasStr = "M";
    inline std::string actionTradeAliasStr = "T";
    inline std::string actionFillAliasStr = "F";
    inline std::string actionClearAliasStr = "R";
    inline std::string actionNoneAliasStr = "N";
    inline bool loaded = false;
}

// matplot++ figure styling, the [matplot] section of config.toml. per-series styling
// (colour, widths, markers) stays on NamedSeries. fontSize is at matplot's 560x420
// reference figure and gets scaled with the figure like everything else
struct MatplotConfig {
    std::string font            = "Helvetica";
    float       fontSize        = 10.f;
    std::string fontWeight      = "normal";
    float       titleFontScale  = 1.1f;   // title size as a multiple of fontSize
    std::string titleFontWeight = "bold";
    bool        grid            = false;
    bool        minorGrid       = false;
    bool        box             = true;
};

inline MatplotConfig kMatplotConfig;

inline CSVMapping kCSVMapping{
    0, 0, 0,
    "",                                     // path (empty until loadConfig)
    {0, 4, 5, 2, 8, 2},                     // ISO-8601 defaults
    true,                                   // skipHeader
    -1, -1,                                 // tsEventCol/rowNumberCol disabled
    -1, "", false,                          // symbol filtering disabled
    -1, "B", "S", "N",                      // aggressor disabled, default aliases
    -1, -1,                                 // resting bid/ask columns disabled
    -1, -1,                                 // bid/ask price columns disabled
    -1, "A", "C", "M", "T", "F", "R", "N",  // action column disabled, default aliases
    0.f, 0.f, 0.f                           // costs
};

// config parsing helpers, after kCSVMapping because colSpec records header names in it
namespace cfgDetail {
    inline bool isIndex(const std::string& v) {
        if (v.empty()) return false;
        std::size_t i = (v[0] == '-' || v[0] == '+') ? 1 : 0;
        if (i == v.size()) return false;
        for (; i < v.size(); ++i) if (v[i] < '0' || v[i] > '9') return false;
        return true;
    }

    /// a column given as an index ("3", "-1") or a header name ("price"). names come back as -1 and
    /// are remembered under `key` until a file is opened and the header can resolve them
    inline int colSpec(const toml::Table& cfg, const std::string& key, int def) {
        std::string v = toml::getString(cfg, "", key, "");
        if (v.empty()) return def;
        if (isIndex(v)) return std::stoi(v);
        kCSVMapping.colNames[key] = v;
        return -1;
    }

    /// "implied_vol:mean" -> column "implied_vol", aggregation Mean. no suffix means Last
    inline ExtraColumn parseExtra(const std::string& name, std::string spec) {
        ExtraColumn e;
        e.name = name;
        auto colon = spec.rfind(':');
        if (colon != std::string::npos) {
            const std::string a = spec.substr(colon + 1);
            const std::pair<const char*, ExtraAgg> aggs[] = {
                {"last", ExtraAgg::Last}, {"first", ExtraAgg::First}, {"sum", ExtraAgg::Sum},
                {"min", ExtraAgg::Min},   {"max", ExtraAgg::Max},     {"mean", ExtraAgg::Mean}};
            for (auto& [k, v] : aggs) if (a == k) { e.agg = v; spec = spec.substr(0, colon); break; }
        }
        if (isIndex(spec)) e.col = std::stoi(spec); else e.header = spec;
        return e;
    }

    /// [filter] value: optional operator (== != < <= > >= ~ !~) then the operand. ~ is "contains",
    /// a bare value is equality, "!" alone is shorthand for !=
    inline RowFilter parseFilter(const std::string& column, std::string v) {
        RowFilter f;
        if (isIndex(column)) f.col = std::stoi(column); else f.header = column;
        const std::pair<const char*, FilterOp> ops[] = {
            {"!~", FilterOp::NotContains}, {"==", FilterOp::Eq}, {"!=", FilterOp::Ne},
            {"<=", FilterOp::Le},          {">=", FilterOp::Ge}, {"~", FilterOp::Contains},
            {"<", FilterOp::Lt},           {">", FilterOp::Gt},  {"=", FilterOp::Eq}, {"!", FilterOp::Ne}};
        for (auto& [sym, op] : ops) {
            const std::size_t len = std::char_traits<char>::length(sym);
            if (v.compare(0, len, sym) == 0) { f.op = op; v = v.substr(len); break; }
        }
        f.operand = v;
        try {
            std::size_t used = 0;
            f.number = std::stod(v, &used);
            f.numeric = used == v.size() && !v.empty();
        } catch (...) {}
        return f;
    }
}

inline void generateDefaultConfig(const char* tomlPath) {
    std::ofstream out(tomlPath);
    out <<
R"(# AZBacktest configuration
# fill in your column mapping and data path, then rebuild

# absolute path to the data file (CSV or Parquet)
path = "PLACEHOLDER"

# column indices (0-indexed)
# tsRecvCol is the timestamp used for bar windowing/sorting - if your data
# doesn't distinguish recv/event time, just point it at whatever column
# carries the timestamp
tsRecvCol = 0
priceCol  = 0
sizeCol   = 0

# set to true if the CSV has a header row to skip
skipHeader = true

# event timestamp + row/sequence number columns, set either to -1 to disable
# on Databento TBBO these are ts_event and sequence
tsEventCol   = -1
rowNumberCol = -1

# symbol filtering (set symbolCol to -1 to disable)
symbolCol  = -1
symbol     = ""
symbolRoll = false

# aggressor/side classification (set aggressor to -1 to disable)
aggressor                = -1
buySideAggressorAlias    = "B"
sellSideAggressorAlias   = "S"
unknownSideAggressorAlias = "N"

# resting (top-of-book) size columns, set to -1 to disable
# these are book snapshots, not traded volume
restingBidCol = -1
restingAskCol = -1

# best bid/ask price columns, set to -1 to disable
# quote snapshots at the time of each row, on Databento TBBO these are
# bid_px_00 / ask_px_00
bidPriceCol = -1
askPriceCol = -1

# action classification 
actionCol   = -1
actionAddAlias = "A"
actionCancelAlias = "C"
actionModifyAlias = "M"
actionTradeAlias = "T"
actionFillAlias = "F"
actionClearAlias = "R"
actionNoneAlias = "N"

# CSV only: skip every row whose action isn't actionTradeAlias (needs actionCol)
# MBP rows are book events, their price is a resting order's, not a trade's, so
# bar highs/lows/volume are only meaningful with this on
tradesOnly = false

# ---- data layout -------------------------------------------------------
# every *Col key above also takes a header name instead of an index, e.g. priceCol = "price"

# how the timestamp column is written: iso8601 (default), epoch_s, epoch_ms, epoch_us, epoch_ns, date
timeFormat = "iso8601"

# field separator ("," default, "tab", ";", "|") and whether "..." quoting is honoured (slower)
delimiter = ","
quoted    = false

# rows with a missing / non-numeric price or timestamp are skipped instead of becoming price 0
skipBadRows = true

# ticks: rows are trades / quotes and get aggregated into timeframe bars (default)
# rows : every row already is a bar. priceCol is the close, openCol / highCol / lowCol are optional
barMode = "ticks"
openCol = -1
highCol = -1
lowCol  = -1

# extra MBP columns, set to -1 to disable
# on Databento MBP-1: flags(10), bid_ct_00(17), ask_ct_00(18), instrument_id(4)
flagsCol      = -1
bidCountCol   = -1
askCountCol   = -1
instrumentCol = -1

# trading costs (all in pts)
commission = 0.0
spread     = 0.0
timingCost = 0.0

# any number of extra numeric columns carried through to DataWindow::extras, read with w.extra("name")
# "column" or "column:agg", agg folds them into a bar: last (default) first sum min max mean
# [extra]
# iv  = "implied_vol:mean"
# oi  = 7

# keep only rows that pass every condition, checked before the symbol logic. key = column (name or index),
# value = optional operator then operand: == != < <= > >= ~ (contains) !~ (doesn't contain), bare = equality
# numbers compare numerically, anything else as text
# [filter]
# action = "T"       # trades only
# symbol = "!~-"     # drop calendar spreads like NQU6-NQZ6
# price  = ">0"

# substring offsets for pulling Y/M/D out of the timestamp column
# defaults match Databento's ISO-8601 format: 2025-06-01T22:00:00.065308005Z
[dateFormat]
yearOffset  = 0
yearLength  = 4
monthOffset = 5
monthLength = 2
dayOffset   = 8
dayLength   = 2

# matplot++ chart styling (openLineChart etc.), colours/widths/markers are set
# per series instead. fontSize is at a 560x420 figure and scales with the real one
[matplot]
font            = "Helvetica"
fontSize        = 10.0
fontWeight      = "normal"   # normal / bold
titleFontScale  = 1.1        # title size as a multiple of fontSize
titleFontWeight = "bold"
grid            = false
minorGrid       = false
box             = true
)";
}

inline void loadConfig(const char* tomlPath = "config.toml") {
    if (cfgDetail::loaded) return;
    cfgDetail::loaded = true;

    {
        std::ifstream check(tomlPath);
        if (!check.is_open()) {
            generateDefaultConfig(tomlPath);
            std::cout << "generated " << tomlPath << " with placeholder values, fill it in and rerun" << std::endl;
            std::exit(0);
        }
    }

    auto cfg = toml::parse(tomlPath);

    kCSVMapping.tsRecvCol = cfgDetail::colSpec(cfg, "tsRecvCol", 0);
    kCSVMapping.priceCol  = cfgDetail::colSpec(cfg, "priceCol", 0);
    kCSVMapping.sizeCol   = cfgDetail::colSpec(cfg, "sizeCol", 0);

    cfgDetail::pathStr = toml::getString(cfg, "", "path");
    kCSVMapping.path   = cfgDetail::pathStr.c_str();

    kCSVMapping.dateFormat.yearOffset  = toml::getInt(cfg, "dateFormat", "yearOffset", 0);
    kCSVMapping.dateFormat.yearLength  = toml::getInt(cfg, "dateFormat", "yearLength", 4);
    kCSVMapping.dateFormat.monthOffset = toml::getInt(cfg, "dateFormat", "monthOffset", 5);
    kCSVMapping.dateFormat.monthLength = toml::getInt(cfg, "dateFormat", "monthLength", 2);
    kCSVMapping.dateFormat.dayOffset   = toml::getInt(cfg, "dateFormat", "dayOffset", 8);
    kCSVMapping.dateFormat.dayLength   = toml::getInt(cfg, "dateFormat", "dayLength", 2);

    kCSVMapping.skipHeader = toml::getBool(cfg, "", "skipHeader", true);

    kCSVMapping.tsEventCol   = cfgDetail::colSpec(cfg, "tsEventCol", -1);
    kCSVMapping.rowNumberCol = cfgDetail::colSpec(cfg, "rowNumberCol", -1);

    kCSVMapping.symbolCol = cfgDetail::colSpec(cfg, "symbolCol", -1);
    cfgDetail::symbolStr  = toml::getString(cfg, "", "symbol");
    kCSVMapping.symbol    = cfgDetail::symbolStr.c_str();
    kCSVMapping.symbolRoll = toml::getBool(cfg, "", "symbolRoll", false);

    kCSVMapping.aggressor = cfgDetail::colSpec(cfg, "aggressor", -1);
    cfgDetail::buySideStr = toml::getString(cfg, "", "buySideAggressorAlias", "B");
    kCSVMapping.buySideAggressorAlias = cfgDetail::buySideStr.c_str();
    cfgDetail::sellSideStr = toml::getString(cfg, "", "sellSideAggressorAlias", "S");
    kCSVMapping.sellSideAggressorAlias = cfgDetail::sellSideStr.c_str();
    cfgDetail::unknownSideStr = toml::getString(cfg, "", "unknownSideAggressorAlias", "N");
    kCSVMapping.unknownSideAggressorAlias = cfgDetail::unknownSideStr.c_str();

    kCSVMapping.restingBidCol = cfgDetail::colSpec(cfg, "restingBidCol", -1);
    kCSVMapping.restingAskCol = cfgDetail::colSpec(cfg, "restingAskCol", -1);

    kCSVMapping.bidPriceCol = cfgDetail::colSpec(cfg, "bidPriceCol", -1);
    kCSVMapping.askPriceCol = cfgDetail::colSpec(cfg, "askPriceCol", -1);

    kCSVMapping.actionCol = cfgDetail::colSpec(cfg, "actionCol", -1);
    cfgDetail::actionAddAliasStr = toml::getString(cfg, "", "actionAddAlias", "A");
    kCSVMapping.actionAddAlias = cfgDetail::actionAddAliasStr.c_str();
    cfgDetail::actionCancelAliasStr = toml::getString(cfg, "", "actionCancelAlias", "C");
    kCSVMapping.actionCancelAlias = cfgDetail::actionCancelAliasStr.c_str();
    cfgDetail::actionModifyAliasStr = toml::getString(cfg, "", "actionModifyAlias", "M");
    kCSVMapping.actionModifyAlias = cfgDetail::actionModifyAliasStr.c_str();
    cfgDetail::actionTradeAliasStr = toml::getString(cfg, "", "actionTradeAlias", "T");
    kCSVMapping.actionTradeAlias = cfgDetail::actionTradeAliasStr.c_str();
    cfgDetail::actionFillAliasStr = toml::getString(cfg, "", "actionFillAlias", "F");
    kCSVMapping.actionFillAlias = cfgDetail::actionFillAliasStr.c_str();
    cfgDetail::actionClearAliasStr = toml::getString(cfg, "", "actionClearAlias", "R");
    kCSVMapping.actionClearAlias = cfgDetail::actionClearAliasStr.c_str();
    cfgDetail::actionNoneAliasStr = toml::getString(cfg, "", "actionNoneAlias", "N");
    kCSVMapping.actionNoneAlias = cfgDetail::actionNoneAliasStr.c_str();
    kCSVMapping.tradesOnly = toml::getBool(cfg, "", "tradesOnly", false);

    {
        const std::string tf = toml::getString(cfg, "", "timeFormat", "iso8601");
        const std::pair<const char*, TimeFormat> formats[] = {
            {"iso8601", TimeFormat::Iso8601},      {"epoch_s", TimeFormat::EpochSeconds},
            {"epoch_ms", TimeFormat::EpochMillis}, {"epoch_us", TimeFormat::EpochMicros},
            {"epoch_ns", TimeFormat::EpochNanos},  {"date", TimeFormat::Date}};
        bool known = false;
        for (auto& [name, fmt] : formats) if (tf == name) { kCSVMapping.timeFormat = fmt; known = true; }
        if (!known) throw std::runtime_error("config: unknown timeFormat \"" + tf +
                         "\", use iso8601, epoch_s, epoch_ms, epoch_us, epoch_ns or date");
    }

    {
        std::string d = toml::getString(cfg, "", "delimiter", ",");
        if (d == "tab" || d == "\\t") kCSVMapping.delimiter = '\t';
        else if (!d.empty())          kCSVMapping.delimiter = d[0];
    }
    kCSVMapping.quoted      = toml::getBool(cfg, "", "quoted", false);
    kCSVMapping.skipBadRows = toml::getBool(cfg, "", "skipBadRows", true);

    kCSVMapping.rowsAreBars = toml::getString(cfg, "", "barMode", "ticks") == "rows";
    kCSVMapping.openCol = cfgDetail::colSpec(cfg, "openCol", -1);
    kCSVMapping.highCol = cfgDetail::colSpec(cfg, "highCol", -1);
    kCSVMapping.lowCol  = cfgDetail::colSpec(cfg, "lowCol", -1);

    kCSVMapping.extras.clear();
    if (auto it = cfg.find("extra"); it != cfg.end())
        for (auto& [name, spec] : it->second) kCSVMapping.extras.push_back(cfgDetail::parseExtra(name, spec));
    if ((int)kCSVMapping.extras.size() > kMaxExtras)
        throw std::runtime_error("config: more than " + std::to_string(kMaxExtras) + " [extra] columns");

    kCSVMapping.filters.clear();
    if (auto it = cfg.find("filter"); it != cfg.end())
        for (auto& [column, v] : it->second) kCSVMapping.filters.push_back(cfgDetail::parseFilter(column, v));

    kCSVMapping.flagsCol      = cfgDetail::colSpec(cfg, "flagsCol", -1);
    kCSVMapping.bidCountCol   = cfgDetail::colSpec(cfg, "bidCountCol", -1);
    kCSVMapping.askCountCol   = cfgDetail::colSpec(cfg, "askCountCol", -1);
    kCSVMapping.instrumentCol = cfgDetail::colSpec(cfg, "instrumentCol", -1);

    kCSVMapping.commision  = toml::getFloat(cfg, "", "commission", 0.f);
    kCSVMapping.spread     = toml::getFloat(cfg, "", "spread", 0.f);
    kCSVMapping.timingCost = toml::getFloat(cfg, "", "timingCost", 0.f);

    // missing keys (or a config from before [matplot] existed) keep matplot's defaults
    auto& mp = kMatplotConfig;
    const MatplotConfig def;
    mp.font            = toml::getString(cfg, "matplot", "font", def.font);
    mp.fontSize        = toml::getFloat(cfg, "matplot", "fontSize", def.fontSize);
    mp.fontWeight      = toml::getString(cfg, "matplot", "fontWeight", def.fontWeight);
    mp.titleFontScale  = toml::getFloat(cfg, "matplot", "titleFontScale", def.titleFontScale);
    mp.titleFontWeight = toml::getString(cfg, "matplot", "titleFontWeight", def.titleFontWeight);
    mp.grid            = toml::getBool(cfg, "matplot", "grid", def.grid);
    mp.minorGrid       = toml::getBool(cfg, "matplot", "minorGrid", def.minorGrid);
    mp.box             = toml::getBool(cfg, "matplot", "box", def.box);
}
