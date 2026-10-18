#pragma once

#include <chrono>

// stopwatch, first getDiff() starts it and returns 0, second returns the ms since the first
// then it resets so the next call starts a fresh pair
//
//   Stopwatch sw;
//   sw.getDiff();
//   doWork();
//   double ms = sw.getDiff();
class Stopwatch {
private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point _start;
    bool _running = false;

public:
    double getDiff() {
        if (!_running) {
            _start = Clock::now();
            _running = true;
            return 0.0;
        }
        _running = false;
        return std::chrono::duration<double, std::milli>(Clock::now() - _start).count();
    }
};
