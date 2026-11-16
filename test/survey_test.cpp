// Medida del analizador (src/radio/survey.cpp) con IQ sinteticos, sin adaptador.
//   survey_test        todos los casos; sale con 1 si alguno falla
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "survey.h"

using namespace fpv;

namespace {
int failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%-64s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

constexpr double kFs = 40e6, kEdge = 16.5e6;
constexpr size_t kN = 480000;  // 12 ms, como la captura de un paso del escaneo
std::mt19937 rng(1);

// ruido blanco de potencia 1 + (si amp > 0) ruido de banda ancha que ocupa todo el paso, encendido en la
// fraccion `on` del tiempo (por rodajas de 0.1 ms)
std::vector<cfs> capture(double amp, double on) {
    std::normal_distribution<float> g(0.f, std::sqrt(0.5f));
    std::vector<cfs> x(kN);
    const size_t slice = 4096;
    for (size_t i = 0; i < kN; i++) {
        x[i] = {g(rng), g(rng)};
        const bool lit = static_cast<double>((i / slice) % 10) < on * 10;  // 117 rodajas: periodo de 10
        if (amp > 0 && lit) x[i] += cfs(g(rng), g(rng)) * static_cast<float>(amp);
    }
    return x;
}

double level(const std::vector<SurveyBin> &s, double lo, double hi, float SurveyBin::*f) {
    double sum = 0;
    int n = 0;
    for (const auto &b : s)
        if (b.mhz > lo && b.mhz < hi) {
            sum += b.*f;
            n++;
        }
    return n ? sum / n : -99;
}
}  // namespace

int main() {
    // C1: un paso entero ocupado (WiFi de 80 MHz) junto a uno limpio: se ve sobre el suelo del limpio
    {
        SurveyGrid g;
        g.reset();
        for (int sweep = 0; sweep < 3; sweep++) {
            auto a = capture(0, 0);
            g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kEdge, g.floor()));
            auto b = capture(std::sqrt(10.0), 1);  // +10 dB en todo el paso
            g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kEdge, g.floor()));
            g.end_sweep();
        }
        const auto s = g.spectrum();
        const double clean = level(s, 5690, 5710, &SurveyBin::mean_db);
        const double wifi = level(s, 5720, 5740, &SurveyBin::mean_db);
        std::printf("  limpio %.1f dB, paso ocupado %.1f dB\n", clean, wifi);
        check(std::fabs(clean) < 1.5 && wifi > 8.5 && wifi < 11.5, "un paso entero ocupado se ve (~+10 dB), no como suelo");
    }
    // A1: rafagas el 30 % del tiempo: duty ~0.3 y pico muy por encima de la media
    {
        SurveyGrid g;
        g.reset();
        auto a = capture(0, 0);
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kEdge, 0));
        auto b = capture(std::sqrt(100.0), 0.3);  // +20 dB el 30 % del tiempo
        g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kEdge, g.floor()));
        g.end_sweep();
        const auto s = g.spectrum();
        const double duty = level(s, 5720, 5740, &SurveyBin::duty);
        const double mean = level(s, 5720, 5740, &SurveyBin::mean_db);
        const double peak = level(s, 5720, 5740, &SurveyBin::peak_db);
        std::printf("  rafagas: duty %.2f, media %.1f dB, pico %.1f dB\n", duty, mean, peak);
        check(duty > 0.25 && duty < 0.35, "rafagas al 30 %: duty ~0.3");
        check(peak - mean > 6, "rafagas: el pico supera la media en mas de 6 dB");
        const double dclean = level(s, 5690, 5710, &SurveyBin::duty);
        check(dclean < 0.02, "paso limpio: duty ~0");
    }
    // A2: lo que se va deja de contar (media deslizante, pico que se apaga)
    {
        SurveyGrid g;
        g.reset();
        auto a = capture(0, 0);
        auto b = capture(std::sqrt(100.0), 0.3);
        auto c = capture(0, 0);
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kEdge, 0));
        g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kEdge, g.floor()));
        g.end_sweep();
        for (int sweep = 0; sweep < 3 * SurveyGrid::kMemory; sweep++) {
            g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kEdge, g.floor()));
            g.merge(survey_measure(c.data(), c.size(), 5730e6, kFs, kEdge, g.floor()));
            g.end_sweep();
        }
        const auto s = g.spectrum();
        const double mean = level(s, 5720, 5740, &SurveyBin::mean_db);
        const double duty = level(s, 5720, 5740, &SurveyBin::duty);
        std::printf("  tras %d barridos sin rafagas: media %.1f dB, duty %.3f, pico %.1f dB\n",
                    3 * SurveyGrid::kMemory, mean, duty, level(s, 5720, 5740, &SurveyBin::peak_db));
        check(mean < 1.5 && duty < 0.02, "una rafaga que se fue deja de contar en ~1 min");
    }
    // DC y bordes fuera; rejilla alineada
    {
        SurveyGrid g;
        g.reset();
        auto a = capture(0, 0);
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kEdge, 0));
        g.end_sweep();
        const auto s = g.spectrum();
        bool dc = false, edge = false;
        for (const auto &b : s) {
            dc |= std::fabs(b.mhz - 5700) < 0.2;
            edge |= std::fabs(b.mhz - 5700) > 16.7;
        }
        check(!dc && !edge && !s.empty(), "sin bins junto a DC ni fuera de +-16.5 MHz");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
