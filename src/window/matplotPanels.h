// @file matplot++ wrapper that bridges the NamedSeries pool concept and matplot++ plots

#pragma once

#include "tooling/seriesPool.h"

#include <vector>
#include <string>
#include <numeric>
#include <iostream>
#include <matplot/matplot.h>

inline void openLineChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 0) {
        std::cerr << "openLineChart: series \"" << series.name << "\" isn't a line series\n";
        return;
    }
    if (series.cols() == 0) return;

    matplot::hold(matplot::on);
    std::vector<std::string> names;

    for (int c = 0; c < series.cols(); c++) {
        matplot::plot(series.data[c])->use_y2(series.onY2)
                                     .color({series.color.r, series.color.g, series.color.b})
                                     .line_width((series.lineWidth < 0) ? 1 : series.lineWidth);
                                     
        names.push_back(series.colName(c));
    }

    matplot::hold(matplot::off);

    matplot::title(series.name);
    if (series.cols() > 1) matplot::legend(names);

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

inline void matplotShow() { matplot::show(); }