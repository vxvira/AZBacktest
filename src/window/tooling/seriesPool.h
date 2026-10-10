#pragma once
#include <string>
#include <vector>
#include <type_traits>
#include <cctype>
#include <cstdio>

#include "../../skins/skinVars.h" // skins::baseColor

#include <string>

namespace seriesPool {

/// @brief rgba color, all -1 means "let the renderer pick"
struct RGBA {
    float r=-1, g=-1, b=-1, a=-1;
    bool isSet() const { return r >= 0; }
};

/// @brief fill in the active skin's base colour when a series was added without
/// one, so plots follow the theme instead of falling through to implot's palette
/// an explicit colour always wins, and if the skin sets no base colour this is
/// a no-op and implot picks as before
inline RGBA resolveColor(RGBA c) {
    if (c.isSet() || !skins::baseColor.isSet()) return c;
    return {skins::baseColor.r, skins::baseColor.g, skins::baseColor.b, skins::baseColor.a};
}

/// @brief map a series type name onto the code stored in NamedSeries::type
/// accepted names are line, bar, heatmap, scatter and errorbar, case insensitive
/// and tolerant of a trailing s. an unrecognised name warns and falls back to line
/// rather than throwing, so one typo can't take a whole backtest down
inline int parseSeriesType(std::string name) {
    for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
    if (name.empty() || name == "line" || name == "lines")      return 0;
    if (name == "bar" || name == "bars")                        return 1;
    if (name == "heatmap" || name == "heatmaps")                return 2;
    if (name == "scatter")                                      return 3;
    if (name == "errorbar" || name == "errorbars" || name == "error") return 4;
    fprintf(stderr, "seriesPool: unknown series type \"%s\", falling back to line\n", name.c_str());
    return 0;
}

/// @brief optional axis labelling for a heatmap
///
/// a heatmap's cells carry no meaning without knowing what each row and column
/// stands for, and the numbers printed in the cells don't say it. leave this
/// default and the plot renders bare (the old behavior).
///
/// xLabels run left to right, yLabels run BOTTOM to top the way a y axis reads,
/// regardless of the fact that the data itself is row-major from the top down.
/// supply one label per column/row, or fewer and the rest go untitled.
struct HeatmapAxes {
    std::vector<std::string> xLabels;
    std::vector<std::string> yLabels;
    std::string xTitle;
    std::string yTitle;
    std::string valueFormat = "%.1f"; ///< printf format for the number drawn in each cell

    bool isSet() const { return !xLabels.empty() || !yLabels.empty()
                             || !xTitle.empty() || !yTitle.empty(); }
};

/// @brief one entry in the global series pool
/// data is column-major: data[col][row], a simple 1D series has one column
struct NamedSeries {
    std::string name;
    std::vector<std::vector<double>> data;      ///< data[column][row]
    std::vector<std::string> columnNames;       ///< optional per-column names
    int type;                                   ///< 0 line, 1 bar, 2 heatmap, 3 scatter, 4 errorbar, set from the type name via parseSeriesType
    RGBA color;                                 ///< all columns share this color, -1 = auto
    bool onY2 = false;                          ///< default axis when added to a panel, right (Y2) vs left (Y1)
    bool xyBars = false;                        ///< true = data[0] is x, data[1] is y (e.g. a histogram) instead of index-based x
    double barWidth = 0.67;                     ///< only used when xyBars is set
    int heatmapRows = 0;                        ///< heatmap row count (type 2 only)
    int heatmapCols = 0;                        ///< heatmap col count (type 2 only)
    HeatmapAxes heatmapAxes;                    ///< optional row/column labelling (type 2 only)
    std::vector<double> errors;                  ///< per-point error magnitude (type 4 only)
    float lineWidth = -1.0;                     ///< line thickness in px, <=0 = let ImPlot pick (appended at the end so existing positional {...} initializers above stay valid)
    int marker = -1;                            ///< point marker, ImPlotMarker_ codes: -1 none, 0 circle, 1 square, 2 diamond, 3 up, 4 down, 5 left, 6 right, 7 cross, 8 plus, 9 asterisk
    float markerSize = -1.0;                    ///< marker size in px, <=0 = renderer default
    float fillAlpha = -1.0;                     ///< bar fill opacity 0..1, <0 = renderer default (imgui only, gnuplot drops it)
    std::string xLabel;                         ///< x axis title for whatever plot this series lands in
    std::string yLabel;                         ///< y axis title, goes on y2 instead when onY2

    int cols() const { return (int)data.size(); }       ///< number of columns (1 for a simple series)
    int rows() const { return data.empty() ? 0 : (int)data[0].size(); } ///< number of data points per column

    /// @brief get display name for a column, falls back to index if unnamed
    std::string colName(int col) const {
        if (col < (int)columnNames.size() && !columnNames[col].empty())
            return columnNames[col];
        return std::to_string(col);
    }
};

/// @brief process-wide pool of series, everything that's been added lives here
/// the add* helpers return a ref to the series they just pushed so style fields
/// can be set inline, e.g. addLine("pnl", v).marker = 0; the ref dies on the next add
inline std::vector<NamedSeries> pool;

/// @brief wipe the pool so you can start fresh (e.g. between backtests)
inline void clear() { pool.clear(); }

/// @brief find a series by name, or nullptr if it doesn't exist
inline NamedSeries* findSeries(const std::string& name) {
    for (auto& s : pool) if (s.name == name) return &s;
    return nullptr;
}

// The pool requires 'line', 'bar', 'heatmap', 'scatter', or 'errorbar' from an older 
//      series system. Removing it (as of writing this) is more complex than simply keeping the structure
//      for that end of the project intact, while refactoring it here

/// @brief shared body for the 1D adders, the only thing that varies is the kind
/// @param kind  type name understood by parseSeriesType
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addColumn(std::string name, std::vector<T> values, const char* kind,
               RGBA color, bool onY2, float lineWidth) {
    std::vector<double> col(values.begin(), values.end());
    NamedSeries s{std::move(name), {std::move(col)}, {}, parseSeriesType(kind), resolveColor(color), onY2};
    s.lineWidth = lineWidth;
    pool.push_back(std::move(s));
    return pool.back();
}

/// @brief shared body for the 2D adders, one column per inner vector
/// @param kind  type name understood by parseSeriesType
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addColumns(std::string name, std::vector<std::vector<T>> values, const char* kind,
                std::vector<std::string> colNames, RGBA color, bool onY2) {
    std::vector<std::vector<double>> cols;
    cols.reserve(values.size());
    for (auto& v : values) {
        cols.emplace_back(v.begin(), v.end());
    }
    pool.push_back({std::move(name), std::move(cols), std::move(colNames),
                    parseSeriesType(kind), resolveColor(color), onY2});
    return pool.back();
}

/// @brief add a 1D line series to the pool
/// @param name   display name for the series
/// @param values the data, any arithmetic type gets converted to double
/// @param color  optional RGBA color, unset picks up the skin's baseColor
/// @param onY2   default axis when this series gets added to a panel, true = right (Y2)
/// @param lineWidth line thickness in px, <=0 = let ImPlot pick
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addLine(std::string name, std::vector<T> values, RGBA color = {}, bool onY2 = false, float lineWidth = -1.0) {
    return addColumn(std::move(name), std::move(values), "line", color, onY2, lineWidth);
}

/// @brief add a multi column line series to the pool, one line per column
/// @param values   vector of columns, each column is a vector of values
/// @param colNames optional names per column (shows up in explorer + plot legend)
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addLine(std::string name, std::vector<std::vector<T>> values,
             std::vector<std::string> colNames = {}, RGBA color = {}, bool onY2 = false) {
    return addColumns(std::move(name), std::move(values), "line", std::move(colNames), color, onY2);
}

/// @brief add a 1D bar series to the pool
/// @param name   display name for the series
/// @param values bar heights, plotted against their index
/// @param color  optional RGBA color, unset picks up the skin's baseColor
/// @param onY2   default axis when this series gets added to a panel, true = right (Y2)
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addBar(std::string name, std::vector<T> values, RGBA color = {}, bool onY2 = false, float lineWidth = -1.0) {
    return addColumn(std::move(name), std::move(values), "bar", color, onY2, lineWidth);
}

/// @brief add a multi column bar series to the pool, one bar set per column
/// @param values   vector of columns, each column is a vector of values
/// @param colNames optional names per column (shows up in explorer + plot legend)
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addBar(std::string name, std::vector<std::vector<T>> values,
            std::vector<std::string> colNames = {}, RGBA color = {}, bool onY2 = false) {
    return addColumns(std::move(name), std::move(values), "bar", std::move(colNames), color, onY2);
}

/// @brief add an (x,y) bar series, e.g. a histogram: xs=bucket value, ys=occurrences
/// @param name     display name for the series
/// @param xs       x-coordinate per bar (e.g. bucket edges)
/// @param ys       bar height per x (e.g. counts), same length as xs
/// @param barWidth width of each bar in x-axis units
template<typename Tx, typename Ty, typename = std::enable_if_t<std::is_arithmetic_v<Tx> && std::is_arithmetic_v<Ty>>>
NamedSeries& addXYBars(std::string name, std::vector<Tx> xs, std::vector<Ty> ys, double barWidth = 0.67,
               RGBA color = {}, bool onY2 = false) {
    std::vector<double> xf(xs.begin(), xs.end());
    std::vector<double> yf(ys.begin(), ys.end());
    NamedSeries s{std::move(name), {std::move(xf), std::move(yf)}, {}, parseSeriesType("bar"), resolveColor(color), onY2};
    s.xyBars = true;
    s.barWidth = barWidth;
    pool.push_back(std::move(s));
    return pool.back();
}

/// @brief add a heatmap series to the pool
/// @param name   display name
/// @param values flat row-major data (rows * cols elements), first row drawn on top
/// @param rows   number of rows
/// @param cols   number of columns
/// @param axes   optional axis titles + per-row/column tick labels, see HeatmapAxes
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addHeatmap(std::string name, std::vector<T> values, int rows, int cols, RGBA color = {},
                HeatmapAxes axes = {}) {
    std::vector<double> flat(values.begin(), values.end());
    NamedSeries s{std::move(name), {std::move(flat)}, {}, parseSeriesType("heatmap"), resolveColor(color), false};
    s.heatmapRows = rows;
    s.heatmapCols = cols;
    s.heatmapAxes = std::move(axes);
    pool.push_back(std::move(s));
    return pool.back();
}

/// @brief add a scatter plot series to the pool
/// @param name   display name
/// @param values data points
/// @param color  optional RGBA color
/// @param onY2   default axis when added to a panel
template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
NamedSeries& addScatter(std::string name, std::vector<T> values, RGBA color = {}, bool onY2 = false) {
    std::vector<double> col(values.begin(), values.end());
    pool.push_back({std::move(name), {std::move(col)}, {}, parseSeriesType("scatter"), resolveColor(color), onY2});
    return pool.back();
}

/// @brief add an (x,y) scatter series, e.g. a correlation: xs=predictor, ys=response
///
/// unlike addScatter, which plots values against their index, this plots each
/// ys[i] at xs[i], which is what you want when the question is how two series
/// move together rather than how one moves over time
/// @param name  display name
/// @param xs    x-coordinate per point
/// @param ys    y-coordinate per point, same length as xs
/// @param color optional RGBA color
/// @param onY2  default axis when added to a panel
template<typename Tx, typename Ty,
         typename = std::enable_if_t<std::is_arithmetic_v<Tx> && std::is_arithmetic_v<Ty>>>
NamedSeries& addXYScatter(std::string name, std::vector<Tx> xs, std::vector<Ty> ys,
                  RGBA color = {}, bool onY2 = false) {
    std::vector<double> xf(xs.begin(), xs.end());
    std::vector<double> yf(ys.begin(), ys.end());
    NamedSeries s{std::move(name), {std::move(xf), std::move(yf)}, {}, parseSeriesType("scatter"), resolveColor(color), onY2};
    s.xyBars = true;
    pool.push_back(std::move(s));
    return pool.back();
}

/// @brief add a line series with error bars to the pool
/// @param name   display name
/// @param values center values per point
/// @param errors error magnitude per point (symmetric, same length as values)
/// @param color  optional RGBA color
/// @param onY2   default axis when added to a panel
template<typename T, typename Te,
         typename = std::enable_if_t<std::is_arithmetic_v<T> && std::is_arithmetic_v<Te>>>
NamedSeries& addErrorBars(std::string name, std::vector<T> values, std::vector<Te> errors,
                  RGBA color = {}, bool onY2 = false) {
    std::vector<double> col(values.begin(), values.end());
    std::vector<double> err(errors.begin(), errors.end());
    NamedSeries s{std::move(name), {std::move(col)}, {}, parseSeriesType("errorbar"), resolveColor(color), onY2};
    s.errors = std::move(err);
    pool.push_back(std::move(s));
    return pool.back();
}

/// @brief batch init for the simple case where each inner vector is its own
/// 1-column series. clears the pool first so this is a full reset
/// @param data  one series per outer vector
/// @param names display name per series (auto-named if shorter than data)
/// @param types type name per series, e.g. "line" or "bar" (defaults to line if shorter)
inline void initSeriesPool(std::vector<std::vector<double>> data,
                           std::vector<std::string> names,
                           std::vector<std::string> types) {
    pool.clear();
    pool.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        std::string n = i < names.size() ? names[i] : "series " + std::to_string(i);
        int t = i < types.size() ? parseSeriesType(types[i]) : parseSeriesType("line");
        pool.push_back({std::move(n), {std::move(data[i])}, {}, t, resolveColor({})});
    }
}

} // namespace seriesPool
