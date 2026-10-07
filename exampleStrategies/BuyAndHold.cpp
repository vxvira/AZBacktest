#include <iostream>
#include <vector>

#include "../src/backtestApi/backtestApi.h"
#include "../src/initSeries.h"
#include "../src/window/window.h"
#include "../src/skins/light.h"
#include "../src/skins/dark.h"
#include "../src/skins/toxic.h"
#include "../src/skins/gilded.h"

int main() {
    loadConfig();

    // TradeApi reads prices.back() as "the price right now", so this vector only
    // ever holds the bar being processed
    std::vector<double> prices;
    MarketData md(kCSVMapping.path);
    DataApi  dataApi(prices, 0.25, 0.50);
    TradeApi tradeApi(prices, 0.25, 0.50);

    const int timeframe = 60;   // seconds per bar
    const int batchSize = 500;  // rows per read, an io detail, not a strategy knob

    dataApi.fetchEOF(timeframe);

    int bar = 0;
    for (;;) {
        DataWindow window = dataApi.requestDataWindow(md, batchSize, timeframe);
        if (window.prices.empty()) break;

        for (std::size_t b = 0; b < window.prices.size(); b++, bar++) {
            if (bar % 5000 == 0)
                std::cout << "  bar " << bar << " / " << dataApi.eof << std::endl;

            prices.assign(1, window.prices[b]);

            // mark the open trade to this bar and stamp the equity curve. the
            // timestamp matters, without it trades close at epoch 0 and anything
            // time bucketed downstream collapses into one bucket
            tradeApi.tick(window.tsRecv[b]);

            // buy the first bar, then just sit in it until closeAll below
            if (!tradeApi.inLong) tradeApi.openLong(bar);
        }
    }
    tradeApi.closeAll();

    auto profit = PnlAnalytics(tradeApi.info).returnProfitOverTime(1440);
    addLine("equity", std::vector<std::vector<double>>{profit}, {"actual"},
            RGBA{0.5f, 0.8f, 0.5f, 1.0f});

    showConsole("Console", skins::dark);
}
