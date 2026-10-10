#pragma once

#include "string"
#include "vector"
#include <cstdlib>

#include "seriesPool.h"

namespace widgetManagement {

/// Widget management helper funcs -- widgets.h includes the logic
///                                   for widget creation, etc.

/// @brief what kind of widget this is, add new kinds here as you build them
enum WidgetKind { SeriesExplorer, StatisticExplorer };

/// @brief a single widget instance living inside a WidgetWindow
/// each widget kind can stash its own state in here (e.g. selectedSeriesIdx
/// for the series explorer)
struct Widget {
    WidgetKind kind;
    std::string label;

    // SeriesExplorer state
    int selectedSeriesIdx = -1;

    // StatisticExplorer state
    std::vector<int> selectedStatIdxs;
};

/// @brief a top-level ImGui window that holds widgets, you get one of these
/// every time you click New -> Widget in the menu bar
struct WidgetWindow {
    std::string id;
    std::vector<Widget> children;
};

/// @brief all active widget windows, rendered each frame by renderWindows()
inline std::vector<WidgetWindow> windows;

/// @brief find a widget window by id, or nullptr if it doesn't exist
inline WidgetWindow* findWindow(const std::string& id) {
    for (auto& w : windows) {
        if (w.id == id) return &w;
    }
    return nullptr;
}

/// @brief register a new widget window; no-op if `id` already exists
inline void newWindow(std::string id) {
    if (findWindow(id) == nullptr) {
        windows.push_back({id, {}});
    }
}

/// @brief lowest id not already in use, so newly-created windows don't collide
/// with ones just restored from the .ini
inline int nextWindowId() {
    int maxId = -1;
    for (auto& w : windows) {
        char* endp = nullptr;
        long v = std::strtol(w.id.c_str(), &endp, 10);
        if (endp != w.id.c_str() && *endp == '\0' && v > maxId) maxId = (int)v;
    }
    return maxId + 1;
}

} // namespace widgetManagement

namespace panelManagement {

/// @brief kind of series a panel can host
enum SeriesKind { Line, Bar, Heatmap, Scatter, ErrorBar };

/// @brief a single series belonging to a panel
struct Series {
    SeriesKind kind;
    std::string label;
    std::vector<double> data;
    bool unbound = false;
    seriesPool::RGBA color;  ///< -1 = let ImPlot pick
    bool legendHidden = false; ///< true = plot but don't show in legend
    bool onY2 = false;         ///< true = plot against the secondary (right) y-axis
    std::vector<double> xs;     ///< non-empty = explicit x per point (e.g. histogram), else index-based
    double barWidth = 0.67;    ///< only used when xs is non-empty
    int heatmapRows = 0;       ///< heatmap row count (type 2 only)
    int heatmapCols = 0;       ///< heatmap col count (type 2 only)
    seriesPool::HeatmapAxes heatmapAxes; ///< optional row/column labelling (Heatmap kind only)
    std::vector<double> errors; ///< per-point error magnitude (ErrorBar kind only)
    float lineWidth = -1.0;    ///< line thickness in px, <=0 = let ImPlot pick (appended at the end so existing positional {...} initializers above stay valid)
    int marker = -1;           ///< ImPlotMarker_ code, -1 = none
    float markerSize = -1.0;   ///< marker size in px, <=0 = let ImPlot pick
    float fillAlpha = -1.0;    ///< bar fill opacity 0..1, <0 = let ImPlot pick
    std::string xLabel;        ///< x axis title, first child that sets one wins
    std::string yLabel;        ///< y axis title, lands on y2 when onY2

    // provenance, used to persist + restore this child from the .ini: which pool
    // series it was pulled from (empty = raw data added via newLine/BarSeries,
    // not restorable) and whether it came from the xyBars or per-column branch
    std::string sourceSeries;
    bool sourceXY = false;
    int sourceCol = -1;
};

/// @brief a panel window that owns any number of child series
struct Panel {
    std::string id;                  ///< unique id; also drives the ImGui window title
    std::vector<Series> children;    ///< series currently attached to this panel
    bool axesLocked = false;         ///< true = x/y axes can't be panned or zoomed
    bool axesAutoFit = true;         ///< true = x/y axes auto-fit to data extents every frame
};

/// @brief all active panels, rendered each frame by renderPanels()
inline std::vector<Panel> panels;

/// @brief find a panel by id, or nullptr if it doesn't exist
inline Panel* findPanel(const std::string& id) {
    for (auto& p : panels) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

/// @brief register a new panel; no-op if `id` already exists
inline void newPanel(std::string id) {
    if (findPanel(id) == nullptr) {
        panels.push_back({id, {}});
    }
}

/// @brief lowest id not already in use, so newly-created panels don't collide
/// with ones just restored from the .ini
inline int nextPanelId() {
    int maxId = -1;
    for (auto& p : panels) {
        char* endp = nullptr;
        long v = std::strtol(p.id.c_str(), &endp, 10);
        if (endp != p.id.c_str() && *endp == '\0' && v > maxId) maxId = (int)v;
    }
    return maxId + 1;
}

} // namespace panelManagement

