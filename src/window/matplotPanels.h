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

inline void openHeatmapChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 2) {
        std::cerr << "openHeatmapChart: series \"" << series.name << "\" isn't a heatmap series\n";
        return;
    }
    if (series.cols() != 1) return; // heatmaps keep everything flat in data[0]
    if (series.onY2) std::cerr << "openHeatmapChart: matplot can't put heatmaps on y2, \"" << series.name << "\" goes on y1\n";

    // rows * cols has to cover the flat vector exactly, or the slicing below reads past the end
    const int R = series.heatmapRows, C = series.heatmapCols;
    const auto& flat = series.data[0];
    if (R <= 0 || C <= 0 || (size_t)R * C != flat.size()) {
        std::cerr << "openHeatmapChart: \"" << series.name << "\" is " << R << "x" << C
                  << " but has " << flat.size() << " values\n";
        return;
    }

    std::vector<std::vector<double>> grid(R);
    for (int i = 0; i < R; i++)
        grid[i].assign(flat.begin() + i * C, flat.begin() + (i + 1) * C);

    matplot::cla();
    matplot::heatmap(grid);
    matplot::colormap(matplot::palette::viridis()); // same map the ImPlot panel pushes

    const auto& ax = series.heatmapAxes;
    matplot::title(series.name);
    if (!ax.xTitle.empty()) matplot::xlabel(ax.xTitle);
    if (!ax.yTitle.empty()) matplot::ylabel(ax.yTitle);

    if (!ax.xLabels.empty()) {
        std::vector<std::string> xl(ax.xLabels);
        xl.resize(C);
        matplot::gca()->x_axis().ticklabels(xl);
    }
    if (!ax.yLabels.empty()) {
        std::vector<std::string> yl(ax.yLabels);
        yl.resize(R);
        std::reverse(yl.begin(), yl.end());
        matplot::gca()->y_axis().ticklabels(yl);
    }

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

// plot() does this automatically, but for some reason other methods dont?
inline std::vector<double> matplotIndexX(size_t n) {
    std::vector<double> x(n);
    std::iota(x.begin(), x.end(), 1.0);
    return x;
}

inline void openScatterChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 3) {
        std::cerr << "openScatterChart: series \"" << series.name << "\" isn't a scatter series\n";
        return;
    }
    if (series.cols() == 0) return;

    matplot::cla();
    matplot::hold(matplot::on);
    std::vector<std::string> names;

    // styling shared by both layouts, scatter defaults to "o" when no marker was picked
    auto style = [&](matplot::line_handle l) {
        l->use_y2(series.onY2);
        if (series.color.isSet()) l->color({series.color.r, series.color.g, series.color.b});
        l->marker(series.marker >= 0 ? matplotMarker(series.marker) : "o");
        if (series.markerSize > 0) l->marker_size(series.markerSize);
    };

    if (series.xyBars) {
        // addXYScatter: data[0] is x, data[1] is y, one cloud
        if (series.cols() < 2 || series.data[0].size() != series.data[1].size()) {
            std::cerr << "openScatterChart: \"" << series.name << "\" x and y lengths don't match\n";
            matplot::hold(matplot::off);
            return;
        }
        style(matplot::scatter(series.data[0], series.data[1]));
    } else {
        // addScatter: each column plotted against its index, like a line without the line
        for (int c = 0; c < series.cols(); c++) {
            style(matplot::scatter(matplotIndexX(series.data[c].size()), series.data[c]));
            names.push_back(series.colName(c));
        }
    }

    matplot::hold(matplot::off);
    matplotDecorate(series, names);

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

inline void openErrorBarChart(const seriesPool::NamedSeries& series, std::string saveTo = "") {
    if (series.type != 4) {
        std::cerr << "openErrorBarChart: series \"" << series.name << "\" isn't an errorbar series\n";
        return;
    }
    if (series.cols() == 0) return;
    if (series.onY2) std::cerr << "openErrorBarChart: matplot can't put error bars on y2, \"" << series.name << "\" goes on y1\n";

    const auto& y = series.data[0];
    matplot::cla();

    if (series.errors.size() != y.size()) {
        // ImPlot skips the bars when errors is empty, do the same and still show the line
        if (!series.errors.empty())
            std::cerr << "openErrorBarChart: \"" << series.name << "\" has " << series.errors.size()
                      << " errors for " << y.size() << " points, drawing without bars\n";
        auto l = matplot::plot(y);
        if (series.color.isSet()) l->color({series.color.r, series.color.g, series.color.b});
        if (series.lineWidth > 0) l->line_width(series.lineWidth);
    } else {
        auto e = matplot::errorbar(matplotIndexX(y.size()), y, series.errors);
        if (series.color.isSet()) e->color({series.color.r, series.color.g, series.color.b});
        if (series.lineWidth > 0) e->line_width(series.lineWidth);
        if (series.marker >= 0)   e->marker(matplotMarker(series.marker));
    }

    matplotDecorate(series, {});

    if (!saveTo.empty()) matplot::save(saveTo);
    matplot::wait();
}

inline void matplotShow() { matplot::show(); }
