// @file matplot++ wrapper that bridges the NamedSeries pool concept and matplot++ plots

#pragma once

#include "tooling/seriesPool.h"

#include <vector>
#include <string>
#include <numeric>
#include <iostream>
#include <matplot/matplot.h>

// ImPlotMarker_ code (what NamedSeries::marker holds) -> matplot marker spec
inline std::string matplotMarker(int marker) {
    static const char* specs[] = {"o", "s", "d", "^", "v", "<", ">", "x", "+", "*"};
    return (marker >= 0 && marker < 10) ? specs[marker] : "";
}

// title, axis labels and legend, shared by every chart kind
inline void matplotDecorate(const seriesPool::NamedSeries& series, const std::vector<std::string>& names) {
    matplot::title(series.name);
    if (!series.xLabel.empty()) matplot::xlabel(series.xLabel);
    if (!series.yLabel.empty()) {
        if (series.onY2) matplot::y2label(series.yLabel);
        else             matplot::ylabel(series.yLabel);
    }
    if (names.size() > 1) matplot::legend(names);
}

inline void openLineChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 0) {
        std::cerr << "openLineChart: series \"" << series.name << "\" isn't a line series\n";
        return;
    }
    if (series.cols() == 0) return;

    matplot::cla();
    matplot::hold(matplot::on);
    std::vector<std::string> names;

    for (int c = 0; c < series.cols(); c++) {
        auto l = matplot::plot(series.data[c]);
        l->use_y2(series.onY2)
          .line_width((series.lineWidth <= 0) ? 1 : series.lineWidth);

        // unset colour is -1s, let matplot's palette pick instead of handing it those
        if (series.color.isSet()) l->color({series.color.r, series.color.g, series.color.b});
        if (series.marker >= 0)   l->marker(matplotMarker(series.marker));
        if (series.markerSize > 0) l->marker_size(series.markerSize);

        names.push_back(series.colName(c));
    }

    matplot::hold(matplot::off);
    matplotDecorate(series, names);

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

inline void openBarChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 1) {
        std::cerr << "openBarChart: series \"" << series.name << "\" isn't a bar series\n";
        return;
    }
    if (series.cols() == 0) return;
    if (series.onY2) std::cerr << "openBarChart: matplot can't put bars on y2, \"" << series.name << "\" goes on y1\n";

    matplot::cla();
    matplot::bars_handle h;
    std::vector<std::string> names;

    if (series.xyBars) {
        // data[0] is x, data[1] is the bar height
        if (series.cols() < 2) return;
        h = matplot::bar(series.data[0], series.data[1]);
        h->bar_width((float)series.barWidth);
    } else if (series.cols() == 1) {
        h = matplot::bar(series.data[0]);
    } else {
        h = matplot::bar(series.data);
        for (int c = 0; c < series.cols(); c++) names.push_back(series.colName(c));
    }

    if (series.color.isSet() && names.size() <= 1)
        h->face_color({series.color.r, series.color.g, series.color.b});
    if (series.lineWidth > 0) h->line_width(series.lineWidth);

    matplotDecorate(series, names);

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

inline void matplotShow() { matplot::show(); }
