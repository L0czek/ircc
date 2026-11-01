#ifndef __TREND_HPP__
#define __TREND_HPP__

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <string>
#include <vector>

/**
 * @brief Time series of one measured quantity (not thread safe; the TUI guards it)
 */
struct TrendSeries {
    using clock = std::chrono::steady_clock;
    struct Point {
        clock::time_point t;
        float v;
    };

    std::string name;
    std::string unit;
    std::deque<Point> points;

    void add(clock::time_point t, float v, clock::duration keep) {
        points.push_back({t, v});
        while (!points.empty() && t - points.front().t > keep) {
            points.pop_front();
        }
    }

    /** @brief Min/max of the points newer than `since`; false if there are none */
    bool range(clock::time_point since, float& lo, float& hi) const {
        lo = std::numeric_limits<float>::max();
        hi = std::numeric_limits<float>::lowest();
        bool any = false;
        for (const auto& p : points) {
            if (p.t >= since) {
                lo = std::min(lo, p.v);
                hi = std::max(hi, p.v);
                any = true;
            }
        }
        return any;
    }
};

#endif
