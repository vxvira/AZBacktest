#pragma once
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_internal.h" // ImGuiSettingsHandler / ImHashStr for ini persistence
#include "implot.h"
#include "string"
#include "vector"
#include <cstdlib>
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "seriesPool.h"

#include "management.h"
#include "tiling.h"

namespace panelManagement {

/// @brief carry the per-series style fields from a pool series onto a panel child
inline void copyStyle(const seriesPool::NamedSeries& s, Series& c) {
    c.lineWidth = s.lineWidth;
    c.marker = s.marker;
    c.markerSize = s.markerSize;
    c.fillAlpha = s.fillAlpha;
    c.xLabel = s.xLabel;
    c.yLabel = s.yLabel;
}

/// @brief build a panel child for one column of a pool series
inline Series buildColumnChild(seriesPool::NamedSeries& s, int col, bool onY2) {
    std::string label = s.cols() == 1 ? s.name : s.name + " [" + s.colName(col) + "]";
    // for large multi-column series (clouds), hide individual legend entries
    bool hide = s.cols() > 10 && col > 0;
    if (hide) label = "##" + s.name + "_" + std::to_string(col);
    SeriesKind kind = s.type == 4 ? ErrorBar : s.type == 3 ? Scatter : s.type == 2 ? Heatmap : (s.type == 1 ? Bar : Line);
    Series c{kind, label, s.data[col], false, s.color, hide, onY2};
    copyStyle(s, c);
    c.heatmapRows = s.heatmapRows;
    c.heatmapCols = s.heatmapCols;
    c.heatmapAxes = s.heatmapAxes;
    if (!s.errors.empty()) c.errors = s.errors;
    c.sourceSeries = s.name;
    c.sourceCol = col;
    return c;
}

/// @brief build a panel child for an xyBars pool series (histogram-style)
inline Series buildXYChild(seriesPool::NamedSeries& s, bool onY2) {
    Series c{s.type == 3 ? Scatter : Bar, s.name, s.data[1], false, s.color, false, onY2, s.data[0], s.barWidth};
    copyStyle(s, c);
    c.sourceSeries = s.name;
    c.sourceXY = true;
    return c;
}

/// @brief place one tick per heatmap cell centre along `axis`
///
/// PlotHeatmap draws into the unit square, so cell i of `count` is centred at
/// (i + 0.5)/count. labels shorter than `count` just leave the rest untitled.
inline void setupHeatmapTicks(ImAxis axis, const std::vector<std::string>& labels, int count) {
    if (labels.empty() || count <= 0) return;
    int n = (int)labels.size() < count ? (int)labels.size() : count;
    std::vector<double> pos;
    std::vector<const char*> text;
    pos.reserve(n); text.reserve(n);
    for (int i = 0; i < n; ++i) {
        pos.push_back((i + 0.5) / count);
        text.push_back(labels[i].c_str());
    }
    ImPlot::SetupAxisTicks(axis, pos.data(), n, text.data());
}

/// @brief render every registered panel for the current ImGui frame
inline void renderPanels() {
    for (int pi = 0; pi < (int)panels.size(); ) {
        auto& p = panels[pi];
        // Title from series names, fall back to id
        std::string winTitle;
        if (p.children.empty()) {
            winTitle = "Plot " + p.id;
        } else {
            for (size_t i = 0; i < p.children.size(); i++) {
                if (p.children[i].legendHidden) continue;
                if (!winTitle.empty()) winTitle += ", ";
                winTitle += p.children[i].label;
            }
            if (winTitle.empty()) winTitle = "Plot " + p.id;
        }
        winTitle += "###panel_" + p.id;
        bool open = true;
        ImGui::Begin(winTitle.c_str(), &open, tiling::apply("panel_" + p.id));

        std::string btnId = "+ Add Series##" + p.id;
        if (ImGui::Button(btnId.c_str())) ImGui::OpenPopup(btnId.c_str());
        if (ImGui::BeginPopup(btnId.c_str())) {
            if (seriesPool::pool.empty()) {
                ImGui::TextDisabled("(no series loaded)"); // if no series was initilized, show this text
            } else { // if a series was initlized, ->
                for (auto& s : seriesPool::pool) {
                    if (ImGui::Selectable(s.name.c_str())) {
                        if (s.xyBars) {
                            p.children.push_back(buildXYChild(s, s.onY2));
                        } else {
                            for (int col = 0; col < s.cols(); col++)
                                p.children.push_back(buildColumnChild(s, col, s.onY2));
                        }
                    }
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        std::string lockId = "Lock axes##lock_" + p.id;
        ImGui::Checkbox(lockId.c_str(), &p.axesLocked);

        ImGui::SameLine();
        std::string autoFitId = "Auto-fit axes##autofit_" + p.id;
        ImGui::Checkbox(autoFitId.c_str(), &p.axesAutoFit);

        // Per-series toggle for which y-axis (left/Y1 vs right/Y2) each
        // series plots against, only shown once there's more than one
        // series to make the choice meaningful
        if (p.children.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("Y2:");
            for (size_t i = 0; i < p.children.size(); i++) {
                auto& c = p.children[i];
                if (c.legendHidden) continue;
                ImGui::SameLine();
                std::string cbId = c.label + "##y2_" + p.id + "_" + std::to_string(i);
                ImGui::Checkbox(cbId.c_str(), &c.onY2);
            }
        }

        // check if this panel has any non-heatmap children
        bool hasStandard = false, hasHeatmap = false;
        for (auto& c : p.children) {
            if (c.unbound) continue;
            if (c.kind == Heatmap) hasHeatmap = true;
            else hasStandard = true;
        }

        if (hasStandard) {
            if (ImPlot::BeginPlot(p.id.c_str(), ImVec2(-1, hasHeatmap ? 0 : -1))) {
                ImPlotAxisFlags lockFlags = p.axesLocked ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None;
                ImPlotAxisFlags fitFlags  = p.axesAutoFit ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
                ImPlotAxisFlags baseFlags = lockFlags | fitFlags;

                ImPlotAxisFlags y2Flags = ImPlotAxisFlags_AuxDefault | baseFlags;
                bool anyOnY2 = false;
                for (auto& c : p.children) if (c.onY2) { anyOnY2 = true; break; }
                if (!anyOnY2) y2Flags |= ImPlotAxisFlags_NoDecorations;

                // axis titles come from the first child on that axis that set one
                const char* xLabel = nullptr; const char* y1Label = nullptr; const char* y2Label = nullptr;
                for (auto& c : p.children) {
                    if (c.unbound || c.kind == Heatmap) continue;
                    if (!xLabel && !c.xLabel.empty()) xLabel = c.xLabel.c_str();
                    const char*& yl = c.onY2 ? y2Label : y1Label;
                    if (!yl && !c.yLabel.empty()) yl = c.yLabel.c_str();
                }

                ImPlot::SetupAxis(ImAxis_X1, xLabel, baseFlags);
                ImPlot::SetupAxis(ImAxis_Y1, y1Label, baseFlags);
                ImPlot::SetupAxis(ImAxis_Y2, y2Label, y2Flags);

                for (auto& c : p.children) {
                    if (c.unbound || c.kind == Heatmap) continue;
                    {
                        ImVec4 cv = c.color.isSet() ? ImVec4(c.color.r, c.color.g, c.color.b, c.color.a) : IMPLOT_AUTO_COL;
                        ImPlot::SetNextLineStyle(cv, c.lineWidth > 0.0 ? c.lineWidth : IMPLOT_AUTO);
                        ImPlot::SetNextFillStyle(cv, c.fillAlpha >= 0.0 ? c.fillAlpha : IMPLOT_AUTO);
                        ImPlot::SetNextMarkerStyle(c.marker >= 0 ? c.marker : IMPLOT_AUTO,
                                                   c.markerSize > 0.0 ? c.markerSize : IMPLOT_AUTO, cv);
                    }
                    ImPlot::SetAxes(ImAxis_X1, c.onY2 ? ImAxis_Y2 : ImAxis_Y1);
                    if (c.kind == Scatter) {
                        if (!c.xs.empty())
                            ImPlot::PlotScatter(c.label.c_str(), c.xs.data(), c.data.data(), (int)c.data.size());
                        else
                            ImPlot::PlotScatter(c.label.c_str(), c.data.data(), (int)c.data.size());
                    }
                    else if (c.kind == ErrorBar) {
                        ImPlot::PlotLine(c.label.c_str(), c.data.data(), (int)c.data.size());
                        if (!c.errors.empty()) {
                            int n = (int)c.data.size();
                            std::vector<double> xs(n);
                            for (int i = 0; i < n; i++) xs[i] = (double)i;
                            ImPlot::PlotErrorBars(c.label.c_str(), xs.data(), c.data.data(), c.errors.data(), n);
                        }
                    }
                    else if (c.kind == Line)
                        ImPlot::PlotLine(c.label.c_str(), c.data.data(), (int)c.data.size());
                    else if (!c.xs.empty())
                        ImPlot::PlotBars(c.label.c_str(), c.xs.data(), c.data.data(), (int)c.data.size(), c.barWidth);
                    else
                        ImPlot::PlotBars(c.label.c_str(), c.data.data(), (int)c.data.size());
                }
                ImPlot::EndPlot();
            }
        }

        for (auto& c : p.children) {
            if (c.kind != Heatmap || c.unbound) continue;
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot(c.label.c_str(), ImVec2(-1, -1), ImPlotFlags_NoLegend)) {
                const auto& ax = c.heatmapAxes;
                if (ax.isSet()) {
                    // PlotHeatmap spans the unit square, so a cell's centre sits at
                    // (i + 0.5)/count along each axis
                    ImPlotAxisFlags hmFlags = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines;
                    ImPlot::SetupAxes(ax.xTitle.empty() ? nullptr : ax.xTitle.c_str(),
                                      ax.yTitle.empty() ? nullptr : ax.yTitle.c_str(),
                                      hmFlags, hmFlags);
                    ImPlot::SetupAxesLimits(0, 1, 0, 1, ImPlotCond_Always);
                    setupHeatmapTicks(ImAxis_X1, ax.xLabels, c.heatmapCols);
                    // yLabels are bottom-up, matching the axis rather than the row order
                    setupHeatmapTicks(ImAxis_Y1, ax.yLabels, c.heatmapRows);
                } else {
                    ImPlotAxisFlags hmFlags = ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock;
                    ImPlot::SetupAxes(nullptr, nullptr, hmFlags, hmFlags);
                }
                ImPlot::PlotHeatmap(c.label.c_str(), c.data.data(),
                    c.heatmapRows, c.heatmapCols, 0, 0,
                    ax.valueFormat.empty() ? nullptr : ax.valueFormat.c_str());
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
        }

        ImGui::End();
        if (!open) { panels.erase(panels.begin() + pi); }
        else { ++pi; }
    }
}

/// @brief split on '|', used to decode the ini's Col=/XY= lines
inline std::vector<std::string> splitPipe(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t p; (p = s.find('|', start)) != std::string::npos; start = p + 1)
        parts.push_back(s.substr(start, p - start));
    parts.push_back(s.substr(start));
    return parts;
}

inline void* iniReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    newPanel(name);
    return (void*)findPanel(name);
}

inline void iniReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    if (!entry) return;
    Panel& p = *(Panel*)entry;
    std::string ln(line);
    size_t eq = ln.find('=');
    if (eq == std::string::npos) return;
    std::string key = ln.substr(0, eq), val = ln.substr(eq + 1);

    if (key == "Locked")  { p.axesLocked  = (val != "0"); return; }
    if (key == "AutoFit") { p.axesAutoFit = (val != "0"); return; }
    if (key != "Col" && key != "XY") return;

    auto parts = splitPipe(val);
    auto* s = seriesPool::findSeries(parts[0]);
    if (!s) return;

    if (key == "XY") {
        if (!s->xyBars || parts.size() < 2) return;
        p.children.push_back(buildXYChild(*s, parts[1] == "1"));
    } else {
        if (parts.size() < 3) return;
        int col = (int)std::strtol(parts[1].c_str(), nullptr, 10);
        if (col < 0 || col >= s->cols()) return;
        p.children.push_back(buildColumnChild(*s, col, parts[2] == "1"));
    }
}

inline void iniWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    for (auto& p : panels) {
        buf->appendf("[%s][%s]\n", handler->TypeName, p.id.c_str());
        buf->appendf("Locked=%d\n", p.axesLocked ? 1 : 0);
        buf->appendf("AutoFit=%d\n", p.axesAutoFit ? 1 : 0);
        for (auto& c : p.children) {
            if (c.sourceSeries.empty()) continue;
            if (c.sourceXY) buf->appendf("XY=%s|%d\n", c.sourceSeries.c_str(), c.onY2 ? 1 : 0);
            else            buf->appendf("Col=%s|%d|%d\n", c.sourceSeries.c_str(), c.sourceCol, c.onY2 ? 1 : 0);
        }
        buf->appendf("\n");
    }
}

/// @brief hook panels into ImGui's own .ini load/save so they auto-restore on
/// startup; call once, before the first ImGui::NewFrame()
inline void registerSettingsHandler() {
    ImGuiSettingsHandler h;
    h.TypeName   = "AZPanel";
    h.TypeHash   = ImHashStr("AZPanel");
    h.ReadOpenFn = iniReadOpen;
    h.ReadLineFn = iniReadLine;
    h.WriteAllFn = iniWriteAll;
    ImGui::AddSettingsHandler(&h);
}

} // namespace panelManagement
