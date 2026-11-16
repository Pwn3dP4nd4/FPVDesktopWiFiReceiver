#include "scan_plan.h"

#include <algorithm>

namespace {
constexpr double kReachMHz = 10;  // portadora a cubrir: |f - centro| <= 10 MHz
}

std::vector<double> scan_plan_for(std::vector<double> mhz) {
    std::sort(mhz.begin(), mhz.end());
    std::vector<double> plan;
    // Voraz: el canal mas bajo sin cubrir abre un paso que cubre hasta 20 MHz por encima de el. El paso va en
    // medio de los canales que cubre (no a +10 MHz fijos): con un solo canal queda centrado en el, y una senal
    // digital de 27 MHz (HDZero) cabe entera en los +-16.5 MHz del paso (src/kp_ext/digital_scan.h).
    size_t i = 0;
    while (i < mhz.size()) {
        const double first = mhz[i];
        size_t j = i;
        while (j + 1 < mhz.size() && mhz[j + 1] <= first + 2 * kReachMHz) j++;
        plan.push_back((first + mhz[j]) / 2 * 1e6);
        i = j + 1;
    }
    // Como upstream: primero las bandas comunes (>= 5645 MHz), luego la baja (L).
    std::stable_partition(plan.begin(), plan.end(), [](double hz) { return hz >= 5640e6; });
    return plan;
}

std::vector<double> survey_plan_for(double lo, double hi) {
    std::vector<double> plan;
    for (double c = lo + 15; c - 15 < hi; c += 30) plan.push_back(c * 1e6);
    return plan;
}

int full_scan_steps() {
    int n = 0;
    for (double c = 5652; c <= 5940; c += 20) n++;
    for (double c = 5370; c <= 5630; c += 20) n++;
    return n;
}
