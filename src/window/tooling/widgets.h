#pragma once
#include "imgui.h"
#include "imgui_internal.h" // ImGuiSettingsHandler / ImHashStr for ini persistence
#include "string"
#include "vector"
#include <cstdlib>

#include "seriesPool.h"
#include "statPool.h"

#include "management.h"
#include "tiling.h"

namespace widgetManagement {

/// @brief renders the popup that lists available widget types, if the user
/// picks one it gets appended to the window's children
/// @param w the widget window to add into
/// @return true if a widget was actually added this frame
inline bool renderAddWidgetPopup(WidgetWindow& w) {
    std::string popupId = "AddWidget##" + w.id;
    if (ImGui::BeginPopup(popupId.c_str())) {
        if (ImGui::Selectable("Series Explorer")) {
            w.children.push_back({SeriesExplorer, "Series Explorer"});
            ImGui::EndPopup();
            return true;
        }
        if (ImGui::Selectable("Statistic Explorer")) {
            w.children.push_back({StatisticExplorer, "Statistic Explorer"});
            ImGui::EndPopup();
            return true;
        }
        ImGui::EndPopup();
    }
    return false;
}


/// @brief the series explorer widget, lets you pick a series from the pool
/// and inspect its values in a scrollable table with per-column stats
/// handles both 1D and multi-column series
/// @param widget    the widget instance (holds which series is selected)
/// @param windowId  parent window id, used to keep ImGui ids unique
inline void renderSeriesExplorer(Widget& widget, const std::string& windowId) {
    std::string uid = "##se_" + windowId + "_" + widget.label;

    // Series picker
    const char* preview = widget.selectedSeriesIdx >= 0
                       && widget.selectedSeriesIdx < (int)seriesPool::pool.size()
        ? seriesPool::pool[widget.selectedSeriesIdx].name.c_str()
        : "(select series)";

    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(("##combo" + uid).c_str(), preview)) {
        for (int i = 0; i < (int)seriesPool::pool.size(); i++) {
            bool selected = (i == widget.selectedSeriesIdx);
            if (ImGui::Selectable(seriesPool::pool[i].name.c_str(), selected)) {
                widget.selectedSeriesIdx = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (widget.selectedSeriesIdx < 0 || widget.selectedSeriesIdx >= (int)seriesPool::pool.size()) return;

    auto& series = seriesPool::pool[widget.selectedSeriesIdx];
    ImGui::SameLine();
    ImGui::TextDisabled("(%d rows x %d cols)", series.rows(), series.cols());

    ImGui::Separator();

    // Per-column stats
    for (int col = 0; col < series.cols(); col++) {
        auto& colData = series.data[col];
        if (colData.empty()) continue;
        double min = colData[0], max = colData[0], sum = 0;
        for (double v : colData) {
            if (v < min) min = v;
            if (v > max) max = v;
            sum += v;
        }
        double avg = sum / (double)colData.size();
        if (series.cols() > 1) {
            ImGui::Text("[%s]", series.colName(col).c_str());
            ImGui::SameLine();
        }
        ImGui::Text("min: %.4f  max: %.4f  avg: %.4f", min, max, avg);
    }
    ImGui::Spacing();

    // Scrollable value table, Index + one column per data column
    int numTableCols = 1 + series.cols();
    if (ImGui::BeginTable(("tbl" + uid).c_str(), numTableCols,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
            ImVec2(-1, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 60.0);
        for (int col = 0; col < series.cols(); col++) {
            std::string header = series.cols() == 1 ? "Value" : series.colName(col);
            ImGui::TableSetupColumn(header.c_str(), ImGuiTableColumnFlags_WidthStretch);
        }
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(series.rows());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", row);
                for (int col = 0; col < series.cols(); col++) {
                    ImGui::TableSetColumnIndex(1 + col);
                    // rows() only counts column 0, a shorter column just leaves blanks
                    if (row < (int)series.data[col].size()) ImGui::Text("%.6f", series.data[col][row]);
                }
            }
        }
        ImGui::EndTable();
    }
}

/// @brief the statistic explorer widget, shows logged stats in a table
/// you pick which stats to display from the add menu
/// @param widget    the widget instance (holds selected stat indices)
/// @param windowId  parent window id for unique ImGui ids
inline void renderStatisticExplorer(Widget& widget, const std::string& windowId) {
    std::string uid = "##ste_" + windowId + "_" + widget.label;

    std::string addId = "+ Add Stat" + uid;
    if (ImGui::Button(addId.c_str())) ImGui::OpenPopup(addId.c_str());
    if (ImGui::BeginPopup(addId.c_str())) {
        if (statPool::pool.empty()) {
            ImGui::TextDisabled("(no stats logged)");
        } else {
            for (int i = 0; i < (int)statPool::pool.size(); i++) {
                bool already = false;
                for (int idx : widget.selectedStatIdxs) {
                    if (idx == i) { already = true; break; }
                }
                if (already) continue;
                if (ImGui::Selectable(statPool::pool[i].name.c_str())) {
                    widget.selectedStatIdxs.push_back(i);
                }
            }
        }
        ImGui::EndPopup();
    }

    if (widget.selectedStatIdxs.empty()) return;

    ImGui::Spacing();
    if (ImGui::BeginTable(("stbl" + uid).c_str(), 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH,
            ImVec2(-1, 0))) {
        ImGui::TableSetupColumn("Stat", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0);
        ImGui::TableHeadersRow();

        for (int idx : widget.selectedStatIdxs) {
            if (idx < 0 || idx >= (int)statPool::pool.size()) continue;
            auto& st = statPool::pool[idx];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", st.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.4f", st.value);
        }
        ImGui::EndTable();
    }
}

/// @brief render all widget windows for this frame, empty windows show a
/// centered "Add widget..." button; once a widget is added it takes over
inline void renderWindows() {
    for (int wi = 0; wi < (int)windows.size(); ) {
        auto& w = windows[wi];
        std::string winTitle = "Widget " + w.id;
        bool open = true;
        ImGui::Begin(winTitle.c_str(), &open, tiling::apply("widget_" + w.id));

        if (w.children.empty()) {
            // Centered "Add widget..." button
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 btnSize(120, 0);
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetCursorPosX() + (avail.x - btnSize.x) * 0.5,
                ImGui::GetCursorPosY() + avail.y * 0.5 - ImGui::GetFrameHeight() * 0.5
            ));
            std::string popupId = "AddWidget##" + w.id;
            if (ImGui::Button(("Add widget...##" + w.id).c_str(), btnSize)) {
                ImGui::OpenPopup(popupId.c_str());
            }
            renderAddWidgetPopup(w);
        } else {
            for (auto& child : w.children) {
                switch (child.kind) {
                    case SeriesExplorer:
                        renderSeriesExplorer(child, w.id);
                        break;
                    case StatisticExplorer:
                        renderStatisticExplorer(child, w.id);
                        break;
                }
            }
        }

        ImGui::End();
        if (!open) { windows.erase(windows.begin() + wi); }
        else { ++wi; }
    }
}

inline void* iniReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    newWindow(name);
    return (void*)findWindow(name);
}

inline void iniReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    if (!entry) return;
    WidgetWindow& w = *(WidgetWindow*)entry;
    std::string ln(line);
    size_t eq = ln.find('=');
    if (eq == std::string::npos || ln.substr(0, eq) != "Widget") return;
    std::string val = ln.substr(eq + 1);
    size_t bar = val.find('|');
    std::string kind = bar == std::string::npos ? val : val.substr(0, bar);
    std::string payload = bar == std::string::npos ? "" : val.substr(bar + 1);

    if (kind == "SeriesExplorer") {
        Widget widget{SeriesExplorer, "Series Explorer"};
        for (int i = 0; i < (int)seriesPool::pool.size(); i++)
            if (seriesPool::pool[i].name == payload) { widget.selectedSeriesIdx = i; break; }
        w.children.push_back(widget);
    } else if (kind == "StatisticExplorer") {
        Widget widget{StatisticExplorer, "Statistic Explorer"};
        for (size_t start = 0; start <= payload.size(); ) {
            size_t comma = payload.find(',', start);
            std::string name = payload.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            for (int i = 0; i < (int)statPool::pool.size(); i++)
                if (!name.empty() && statPool::pool[i].name == name) { widget.selectedStatIdxs.push_back(i); break; }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        w.children.push_back(widget);
    }
}

inline void iniWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    for (auto& w : windows) {
        buf->appendf("[%s][%s]\n", handler->TypeName, w.id.c_str());
        for (auto& c : w.children) {
            if (c.kind == SeriesExplorer) {
                std::string name = (c.selectedSeriesIdx >= 0 && c.selectedSeriesIdx < (int)seriesPool::pool.size())
                    ? seriesPool::pool[c.selectedSeriesIdx].name : "";
                buf->appendf("Widget=SeriesExplorer|%s\n", name.c_str());
            } else {
                std::string joined;
                for (int idx : c.selectedStatIdxs) {
                    if (idx < 0 || idx >= (int)statPool::pool.size()) continue;
                    if (!joined.empty()) joined += ",";
                    joined += statPool::pool[idx].name;
                }
                buf->appendf("Widget=StatisticExplorer|%s\n", joined.c_str());
            }
        }
        buf->appendf("\n");
    }
}

/// @brief hook widget windows into ImGui's own .ini load/save so they
/// auto-restore on startup; call once, before the first ImGui::NewFrame()
inline void registerSettingsHandler() {
    ImGuiSettingsHandler h;
    h.TypeName   = "AZWidget";
    h.TypeHash   = ImHashStr("AZWidget");
    h.ReadOpenFn = iniReadOpen;
    h.ReadLineFn = iniReadLine;
    h.WriteAllFn = iniWriteAll;
    ImGui::AddSettingsHandler(&h);
}

} // namespace widgetManagement
