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

    std::vector<double> x(series.rows());          // shared x: 1..n
    std::iota(x.begin(), x.end(), 1.0);

    matplot::hold(matplot::on);                    // keep adding lines to the same axes
    std::vector<std::string> names;
    for (int c = 0; c < series.cols(); c++) {
        matplot::plot(x, series.data[c]);
        names.push_back(series.colName(c));        // falls back to "0", "1", ... if unnamed
    }
    matplot::hold(matplot::off);

    matplot::title(series.name);
    if (series.cols() > 1) matplot::legend(names);

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::show();
}