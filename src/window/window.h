#pragma once
#include <string>
#include <vector>

void showConsole(const char* title, void (skin)());

void addPanel(const std::string& id, const std::vector<std::string>& series);
void addStatWidget(const std::string& id);

void setTiling(bool on);
void setTileGrid(int cols, int rows);
void tileWindow(const std::string& key, int col, int row, int colSpan = 1, int rowSpan = 1);
