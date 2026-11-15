#include "scan_plan.h"

#include <algorithm>

namespace {
constexpr double kReachMHz = 10;  // portadora a cubrir: |f - centro| <= 10 MHz
}

std::vector<double> scan_plan_for(std::vector<double> mhz) {
    std::sort(mhz.begin(), mhz.end());
    std::vector<double> plan;
    // Voraz: el canal mas bajo sin cubrir fija un paso a +10 MHz, que cubre hasta 20 MHz por encima de el.
    double coveredTo = -1;
    for (double f : mhz) {
        if (f <= coveredTo) continue;
        const double c = f + kReachMHz;
        plan.push_back(c * 1e6);
        coveredTo = c + kReachMHz;
    }
    // Como upstream: primero las bandas comunes (>= 5645 MHz), luego la baja (L).
    std::stable_partition(plan.begin(), plan.end(), [](double hz) { return hz >= 5640e6; });
    return plan;
}

int full_scan_steps() {
    int n = 0;
    for (double c = 5652; c <= 5940; c += 20) n++;
    for (double c = 5370; c <= 5630; c += 20) n++;
    return n;
}
