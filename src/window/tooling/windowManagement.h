#pragma once
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_internal.h" 
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <string>

#include "../../skins/skinVars.h" // skins::gridStepX/Y / gridColor / baseColor

namespace windowManagement {

/// @brief layout file the console loads/saves, point a study at its own so it doesnt drag in every old panel from the shared one
inline std::string iniFile = "imgui.ini";

inline GLFWwindow* createWindow(void (skin)()) {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    static GLFWwindow* window = glfwCreateWindow(1280, 720, "AZBacktest", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = iniFile.c_str();
    skin();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    return window;
}

inline void onClose(GLFWwindow* window) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

inline void startFrame() {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

/// @brief position and size of the imgui window the user is currently dragging
struct MovingWindowInfo {
    float x = 0, y = 0; // top left corner in main window pixel coords
    float w = 0, h = 0; // size in pixels
};

inline float workAreaTop();

/// @brief the window being dragged this frame, resolved to its root since
/// dragging a child moves the root, or nullptr when nothing is moving
inline ImGuiWindow* movingWindowPtr() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    ImGuiWindow* win = g ? g->MovingWindow : nullptr;
    if (win && win->RootWindow) win = win->RootWindow;
    return win;
}

/// @brief find where the imgui window being moved this frame sits
/// call it after startFrame(), imgui resolves the drag during NewFrame()
/// @return all zeroes when nothing is being dragged
inline MovingWindowInfo movingWindow() {
    MovingWindowInfo info;
    if (ImGuiWindow* win = movingWindowPtr()) {
        info.x = win->Pos.x;  info.y = win->Pos.y;
        info.w = win->Size.x; info.h = win->Size.y;
    }
    return info;
}

/// @brief outline drag, the dragged window is pinned to the spot it was picked
/// up from so it never follows the cursor, and only drops to where the cursor
/// left it once the mouse comes up
/// call once per frame after startFrame(), the returned rect is the drop target
/// and is what you want to draw the outline around
/// @return the pending drop rect, all zeroes when nothing is being dragged
inline MovingWindowInfo holdMovingWindow() {
    static ImGuiID heldId = 0;
    static ImVec2 dropAt(0, 0);

    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return {};

    if (ImGuiWindow* win = movingWindowPtr()) {
        ImVec2 origin = g->IO.MouseClickedPos[0] - g->ActiveIdClickOffset;  
        ImVec2 delta  = g->IO.MousePos - g->IO.MouseClickedPos[0];          

        // snap per axis, a step of zero or less means that axis is left free
        // rather than divided by zero
        const ImVec2 target = origin + delta;
        dropAt.x = skins::gridStepX > 0.0f
                 ? ImFloor(target.x / skins::gridStepX) * skins::gridStepX : target.x;
        dropAt.y = skins::gridStepY > 0.0f
                 ? ImFloor(target.y / skins::gridStepY) * skins::gridStepY : target.y;

        // snapping down can land the outline under the menu bar, so clamp after
        // it rather than before, otherwise the snap would undo the clamp
        const float top = workAreaTop();
        if (dropAt.y < top) dropAt.y = top;
        heldId = win->ID;

        ImGui::SetWindowPos(win, ImVec2(g->IO.MouseClickedPos[0].x - g->ActiveIdClickOffset.x,
                                        g->IO.MouseClickedPos[0].y - g->ActiveIdClickOffset.y),
                            ImGuiCond_Always);

        return {dropAt.x, dropAt.y, win->Size.x, win->Size.y};
    }

    if (heldId != 0) {
        if (ImGuiWindow* win = ImGui::FindWindowByID(heldId))
            ImGui::SetWindowPos(win, dropAt, ImGuiCond_Always);
        heldId = 0;
    }
    return {};
}

/// @brief which edges the active resize handle drives, all false when nothing
/// on this window is being resized
struct ResizeEdges {
    bool left = false, right = false, up = false, down = false;
    bool any() const { return left || right || up || down; }
};

/// @brief work out which resize handle of win currently holds the active id
/// corner numbering is imgui's own resize_grip_def order, which is 0 lower right,
/// 1 lower left, 2 upper left, 3 upper right, not the clockwise order you'd guess
inline ResizeEdges activeResizeEdges(ImGuiWindow* win) {
    ResizeEdges e;
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !win || !g->ActiveId) return e;

    for (int n = 0; n < 4; n++) {
        if (g->ActiveId != ImGui::GetWindowResizeCornerID(win, n)) continue;
        e.right = (n == 0 || n == 3);
        e.left  = (n == 1 || n == 2);
        e.down  = (n == 0 || n == 1);
        e.up    = (n == 2 || n == 3);
        return e;
    }
    if      (g->ActiveId == ImGui::GetWindowResizeBorderID(win, ImGuiDir_Left))  e.left  = true;
    else if (g->ActiveId == ImGui::GetWindowResizeBorderID(win, ImGuiDir_Right)) e.right = true;
    else if (g->ActiveId == ImGui::GetWindowResizeBorderID(win, ImGuiDir_Up))    e.up    = true;
    else if (g->ActiveId == ImGui::GetWindowResizeBorderID(win, ImGuiDir_Down))  e.down  = true;
    return e;
}

/// @brief pull one axis of a rect onto the grid so it spans whole cells
///
/// both edges land on a grid line, which is what makes the window a whole number
/// of squares rather than merely having the dragged edge aligned. the edge under
/// the cursor floors onto a line so it tracks the drag, the opposite edge rounds
/// to the nearest one, a no-op once a window is aligned and a one time
/// correction while it isn't
///
/// the span is then widened to at least one cell, and to imgui's own minimum
/// rounded up to a whole number of cells, growing away from the dragged edge so
/// the edge being held stays put
/// @param step <=0 leaves this axis alone entirely
inline void snapAxisToGrid(float& lo, float& hi, float step,
                           bool loDriven, bool hiDriven, float minSpan) {
    if (step <= 0.0f) return;

    lo = loDriven ? ImFloor(lo / step) * step : IM_ROUND(lo / step) * step;
    hi = hiDriven ? ImFloor(hi / step) * step : IM_ROUND(hi / step) * step;

    float cells = ImFloor(minSpan / step);
    if (cells * step < minSpan) cells += 1.0f;  // round the minimum up, never down
    if (cells < 1.0f) cells = 1.0f;             // never collapse to nothing

    const float span = cells * step;
    if (hi - lo < span) {
        if (loDriven) lo = hi - span;
        else          hi = lo + span;
    }
}

/// @brief pull a window being resized onto the grid, so it always ends up
/// covering a whole number of grid squares rather than part of one
/// every edge lands on a grid line, not just the one under the cursor, which is
/// what forces the size itself to be a multiple of the step. a step of zero
/// leaves that axis free
/// call once per frame after startFrame(), same as holdMovingWindow()
/// @return true while a resize is being snapped
inline bool snapResizingWindow() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !g->ActiveIdWindow) return false;

    ImGuiWindow* win = g->ActiveIdWindow;
    const ResizeEdges e = activeResizeEdges(win);
    if (!e.any()) return false;

    ImVec2 mn = win->Pos;
    ImVec2 mx = ImVec2(win->Pos.x + win->SizeFull.x, win->Pos.y + win->SizeFull.y);

    const ImVec2 minSize = ImGui::GetStyle().WindowMinSize;
    snapAxisToGrid(mn.x, mx.x, skins::gridStepX, e.left, e.right, minSize.x);
    snapAxisToGrid(mn.y, mx.y, skins::gridStepY, e.up,   e.down,  minSize.y);

    ImGui::SetWindowPos(win, mn, ImGuiCond_Always);
    ImGui::SetWindowSize(win, ImVec2(mx.x - mn.x, mx.y - mn.y), ImGuiCond_Always);
    return true;
}

/// @brief top of the usable area, i.e. just under the main menu bar
/// imgui shrinks the viewport work area by whatever BeginMainMenuBar took, so
/// this follows the bar's real height instead of a hardcoded guess
inline float workAreaTop() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    return vp ? vp->WorkPos.y : 0.0f;
}

/// @brief keep every top level window fully inside the usable area
inline void clampWindowsToWorkArea() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return;

    const ImVec2 wmin = vp->WorkPos;
    const ImVec2 wmax = ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                               vp->WorkPos.y + vp->WorkSize.y);

    for (ImGuiWindow* win : g->Windows) {
        if (!win || !win->WasActive) continue;
        // the menu bar sits above the work area by definition, and popups,
        // tooltips and children are placed by their parent, not by the user
        if (win->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip
                        | ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoMove)) continue;

        // the lower bound wins when the window is bigger than the work area, so
        // an oversized window parks at the top left instead of going negative
        float x = win->Pos.x, y = win->Pos.y;
        const float maxX = wmax.x - win->Size.x;
        const float maxY = wmax.y - win->Size.y;
        if (x > maxX)   x = maxX;
        if (x < wmin.x) x = wmin.x;
        if (y > maxY)   y = maxY;
        if (y < wmin.y) y = wmin.y;

        if (x != win->Pos.x || y != win->Pos.y)
            ImGui::SetWindowPos(win, ImVec2(x, y), ImGuiCond_Always);
    }
}

inline void endFrame(GLFWwindow* window) {
    ImGui::Render();
    int w, h; glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    const ImVec4& bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    glClearColor(bg.x, bg.y, bg.z, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

} // namespace windowManagement
