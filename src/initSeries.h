#include "window/tooling/seriesPool.h"
#include "window/tooling/statPool.h"

// re-export so strategies can use these without namespace qualifiers
using seriesPool::RGBA;
using seriesPool::addLine;
using seriesPool::addBar;
using seriesPool::addScatter;
using seriesPool::addErrorBars;
using seriesPool::addXYBars;
using seriesPool::addXYScatter;
using seriesPool::addXYLine;
using seriesPool::addHeatmap;
using seriesPool::initSeriesPool;
using statPool::addStat;
