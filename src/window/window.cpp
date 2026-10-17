/**
 * @file window.cpp
 * @brief upstream exposure to internal panel / window management systems
 */


#include <iostream>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "implot.h"
#include "vector"
#include "string"

#include "tooling/windowManagement.h"
#include "tooling/windowRendering.h"
#include "tooling/panels.h"
#include "tooling/widgets.h"
#include "tooling/transforms.h"
#include "tooling/tiling.h"

///@name Panel Series API
///@{

/// @brief append a line series with data already in hand
/// @param panelId panel to add into (no-op if not found)
/// @param label   legend label
/// @param data    values plotted in order
void newLineSeries(std::string panelId, std::string label, std::vector<double> data) {
    auto* panel = panelManagement::findPanel(panelId);
    if (panel) panel->children.push_back({panelManagement::Line, label, data});
}

/// @brief append a bar series with data already in hand
/// @param panelId panel to add into (no-op if not found)
/// @param label   legend label
/// @param data    bar heights plotted in order
void newBarSeries(std::string panelId, std::string label, std::vector<double> data) {
    auto* panel = panelManagement::findPanel(panelId);
    if (panel) panel->children.push_back({panelManagement::Bar, label, data});
}

/// @brief unbound line overload, renders a picker button until you choose a series from the pool
/// @param panelId panel to add into (no-op if not found)
void newLineSeries(std::string panelId) {
    auto* panel = panelManagement::findPanel(panelId);
    if (panel) panel->children.push_back({panelManagement::Line, "", {}, true});
}

/// @brief unbound bar overload, renders a picker button until you choose a series from the pool
/// @param panelId panel to add into (no-op if not found)
void newBarSeries(std::string panelId) {
    auto* panel = panelManagement::findPanel(panelId);
    if (panel) panel->children.push_back({panelManagement::Bar, "", {}, true});
}

/// @brief make a panel from pool series before the console opens, so a study
/// lays out its own charts instead of you clicking them together every run
/// @param panelId panel id, also the plot title and the tiling key "panel_<id>"
/// @param series  pool series names, every column gets added, unknown ones get skipped
void addPanel(const std::string& panelId, const std::vector<std::string>& series) {
    panelManagement::newPanel(panelId);
    auto* panel = panelManagement::findPanel(panelId);
    for (auto& name : series) {
        auto* s = seriesPool::findSeries(name);
        if (!s) { std::cerr << "addPanel: no series called \"" << name << "\"\n"; continue; }
        if (s->xyBars) panel->children.push_back(panelManagement::buildXYChild(*s, s->onY2));
        else for (int col = 0; col < s->cols(); col++)
            panel->children.push_back(panelManagement::buildColumnChild(*s, col, s->onY2));
    }
}

/// @brief widget window with a stat explorer showing every stat logged so far
/// @param id window id, tiling key is "widget_<id>"
void addStatWidget(const std::string& id) {
    widgetManagement::newWindow(id);
    auto* w = widgetManagement::findWindow(id);
    widgetManagement::Widget widget{widgetManagement::StatisticExplorer, "Statistic Explorer"};
    for (int i = 0; i < (int)statPool::pool.size(); i++) widget.selectedStatIdxs.push_back(i);
    w->children.push_back(widget);
}

///@}

///@name Tiling API
///@{

/// @brief tiling on = windows get auto laid out, no dragging/resizing, no top bar
void setTiling(bool on) { tiling::enabled = on; }

/// @brief fix the grid size, 0 on either axis lets it pick
void setTileGrid(int cols, int rows) { tiling::gridCols = cols; tiling::gridRows = rows; }

/// @brief force a window into a cell, everything else fills around it
/// @param key "panel_<id>", "widget_<id>", "transform_<id>" or "variable_<id>"
void tileWindow(const std::string& key, int col, int row, int colSpan, int rowSpan) {
    tiling::pins[key] = {col, row, colSpan < 1 ? 1 : colSpan, rowSpan < 1 ? 1 : rowSpan};
}

///@}

std::vector<px> renderBuffer; // pixels to render
std::vector<px> gridBuffer; // draw *under* the imgui stuff, unlike renderBuffer

/// live position of whichever imgui window is being dragged, refreshed once per
/// frame inside showConsole, inactive when nothing is being moved
windowManagement::MovingWindowInfo movingWindow;

/// @brief open the main console window and run the render loop until close
/// @param title window title (currently unused, window gets titled at creation)
void showConsole(const char* title, void (skin)()) {
    static GLFWwindow* window = windowManagement::createWindow(skin);
    // order matters: imgui writes .ini sections in registration order and reads
    // them back the same way. transforms have to come first because they put
    // their derived series into the pool, and panels resolve series by name
    // while reading their own section. variables go before widgets for the same
    // reason, they put their numbers in the stat pool and the statistic
    // explorer resolves those by name
    transformManagement::registerSettingsHandler();
    transformManagement::registerVariableSettingsHandler();
    panelManagement::registerSettingsHandler();
    widgetManagement::registerSettingsHandler();

    px pxTemplate;
    pxTemplate.win = window;
    pxTemplate.r = 0.3;
    pxTemplate.g = 0.3;
    pxTemplate.b = 0.3;
    pxTemplate.a = 0.4;

    while (!glfwWindowShouldClose(window)) {
        windowManagement::startFrame();
        movingWindow = windowManagement::holdMovingWindow();
        windowManagement::snapResizingWindow();
        windowManagement::clampWindowsToWorkArea();

        // csv reload, F5 works in tiling mode too where there's no menu bar.
        // transforms re-run before the panel refresh so their outputs land too
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) seriesPool::csvReloadRequested = true;
        if (seriesPool::csvReloadRequested) {
            seriesPool::csvReloadRequested = false;
            seriesPool::reloadCsvSeries();
            transformManagement::restorePublished();
            transformManagement::restorePublishedVariables();
            panelManagement::refreshFromPool();
        }

        if (tiling::enabled) {
            std::vector<std::string> keys;
            for (auto& p : panelManagement::panels)         keys.push_back("panel_" + p.id);
            for (auto& w : widgetManagement::windows)       keys.push_back("widget_" + w.id);
            for (auto& t : transformManagement::transforms) keys.push_back("transform_" + t.id);
            for (auto& v : transformManagement::variables)  keys.push_back("variable_" + v.id);
            tiling::plan(keys);
        }

        panelManagement::renderPanels();
        widgetManagement::renderWindows();
        transformManagement::renderTransforms();
        transformManagement::renderVariables();
        if (!tiling::enabled && ImGui::BeginMainMenuBar()) {
            if (ImGui::MenuItem("Chart")) {
                std::string id = std::to_string(panelManagement::nextPanelId());
                panelManagement::newPanel(id);
                newLineSeries(id);
            }
            if (ImGui::MenuItem("Widget")) {
                widgetManagement::newWindow(std::to_string(widgetManagement::nextWindowId()));
            }
            if (ImGui::MenuItem("Series Transform")) {
                transformManagement::newTransform(
                    std::to_string(transformManagement::nextTransformId()));
            }
            if (ImGui::MenuItem("Variable Transform")) {
                transformManagement::newVariable(
                    std::to_string(transformManagement::nextVariableId()));
            }
            if (ImGui::MenuItem("Reload CSVs", "F5")) seriesPool::csvReloadRequested = true;

            ImGui::EndMainMenuBar();
        }

        if (movingWindow.w > 0) {
            const int left   = static_cast<int>(movingWindow.x);
            const int top    = static_cast<int>(movingWindow.y);
            const int right  = left + static_cast<int>(movingWindow.w) - 1;
            const int bottom = top + static_cast<int>(movingWindow.h) - 1;

            constexpr int thickness = 5;
            for (int t = 0; t < thickness; t++) {
                for (int x = left; x <= right; x++) {
                    pxTemplate.px = static_cast<float>(x);
                    pxTemplate.py = static_cast<float>(top + t);
                    renderBuffer.push_back(pxTemplate);
                    pxTemplate.py = static_cast<float>(bottom - t);
                    renderBuffer.push_back(pxTemplate);
                }
                for (int y = top + thickness; y <= bottom - thickness; y++) {
                    pxTemplate.py = static_cast<float>(y);
                    pxTemplate.px = static_cast<float>(left + t);
                    renderBuffer.push_back(pxTemplate);
                    pxTemplate.px = static_cast<float>(right - t);
                    renderBuffer.push_back(pxTemplate);
                }
            }
        }

        {
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            static float lastW = -1, lastH = -1, lastStepX = -1, lastStepY = -1;
            if (disp.x != lastW || disp.y != lastH
                || skins::gridStepX != lastStepX || skins::gridStepY != lastStepY) {
                lastW = disp.x; lastH = disp.y;
                lastStepX = skins::gridStepX; lastStepY = skins::gridStepY;
                gridBuffer.clear();
                px gp;
                gp.win = window;
                gp.r = skins::gridColor.x; gp.g = skins::gridColor.y;
                gp.b = skins::gridColor.z; gp.a = skins::gridColor.w;
                // gridStepX spaces the vertical lines, gridStepY the horizontal
                // ones, either can be off on its own
                if (skins::gridStepX > 0.0f) {
                    for (float x = 0; x < disp.x; x += skins::gridStepX) {
                        gp.px = x;
                        for (int y = 0; y < (int)disp.y; y++) {
                            gp.py = (float)y;
                            gridBuffer.push_back(gp);
                        }
                    }
                }
                if (skins::gridStepY > 0.0f) {
                    for (float y = 0; y < disp.y; y += skins::gridStepY) {
                        gp.py = y;
                        for (int x = 0; x < (int)disp.x; x++) {
                            gp.px = (float)x;
                            gridBuffer.push_back(gp);
                        }
                    }
                }
            }
        }

        // grid goes under the windows, the drag outline goes over them
        for (int i=0; i<gridBuffer.size(); i++) {
            draw_px_under(gridBuffer[i]);
        }
        for (int i=0; i<renderBuffer.size(); i++) {
            draw_px(renderBuffer[i]);
        }
        renderBuffer = {};

        windowManagement::endFrame(window);
    }
    windowManagement::onClose(window);
}
