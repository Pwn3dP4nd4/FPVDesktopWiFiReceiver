// Medida del analizador (src/radio/survey.cpp) con IQ sinteticos, sin adaptador.
//   survey_test        todos los casos; sale con 1 si alguno falla
#include <algorithm>
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

constexpr double kFs = 40e6, kReach = 10e6;  // cada paso mide [-10, +10) MHz (src/radio/radio.cpp)
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
            g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, g.reference(5700e6)));
            auto b = capture(std::sqrt(10.0), 1);  // +10 dB en todo el paso
            g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kReach, g.reference(5730e6)));
            g.end_sweep();
        }
        const auto s = g.spectrum();
        const double clean = level(s, 5690, 5710, &SurveyBin::mean_db);
        const double wifi = level(s, 5720, 5740, &SurveyBin::mean_db);
        std::printf("  limpio %.1f dB, paso ocupado %.1f dB\n", clean, wifi);
        check(std::fabs(clean) < 1.5 && wifi > 6.5 && wifi < 11.5, "un paso entero ocupado se ve (+10 dB menos la pendiente)");
    }
    // A1: rafagas el 30 % del tiempo: duty ~0.3 y pico muy por encima de la media
    {
        SurveyGrid g;
        g.reset();
        auto a = capture(0, 0);
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0));
        auto b = capture(std::sqrt(100.0), 0.3);  // +20 dB el 30 % del tiempo
        g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kReach, g.reference(5730e6)));
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
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0));
        g.merge(survey_measure(b.data(), b.size(), 5730e6, kFs, kReach, g.reference(5730e6)));
        g.end_sweep();
        for (int sweep = 0; sweep < 3 * SurveyGrid::kMemory; sweep++) {
            g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, g.reference(5700e6)));
            g.merge(survey_measure(c.data(), c.size(), 5730e6, kFs, kReach, g.reference(5730e6)));
            g.end_sweep();
        }
        const auto s = g.spectrum();
        const double mean = level(s, 5720, 5740, &SurveyBin::mean_db);
        const double duty = level(s, 5720, 5740, &SurveyBin::duty);
        std::printf("  tras %d barridos sin rafagas: media %.1f dB, duty %.3f, pico %.1f dB\n",
                    3 * SurveyGrid::kMemory, mean, duty, level(s, 5720, 5740, &SurveyBin::peak_db));
        check(mean < 1.5 && duty < 0.02, "una rafaga que se fue deja de contar en ~1 min");
    }
    // suelo propio del chip que sube poco a poco (0.8 dB por paso, como el 8812BU de 5950 a 5640 MHz): plano
    {
        SurveyGrid g;
        g.reset();
        for (int sweep = 0; sweep < 3; sweep++) {
            for (int st = 0; st < 10; st++) {
                const double c = 5650e6 + st * 30e6;
                auto x = capture(0, 0);
                const float gain = static_cast<float>(std::pow(10.0, 0.8 * (10 - st) / 20));
                for (auto &v : x) v *= gain;
                g.merge(survey_measure(x.data(), x.size(), c, kFs, kReach, g.reference(c)));
            }
            g.end_sweep();
        }
        const auto s = g.spectrum();
        double worst = 0, duty = 0;
        for (const auto &b : s) {
            worst = std::max(worst, static_cast<double>(b.mean_db));
            duty = std::max(duty, static_cast<double>(b.duty));
        }
        std::printf("  suelo con pendiente de 8 dB: peor media %.1f dB, duty max %.2f\n", worst, duty);
        check(worst < 1.5 && duty < 0.05, "la pendiente del suelo del chip no parece interferencia");
    }
    // DC y bordes fuera; rejilla alineada
    {
        SurveyGrid g;
        g.reset();
        auto a = capture(0, 0);
        g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0));
        g.end_sweep();
        const auto s = g.spectrum();
        bool dc = false, edge = false;
        for (const auto &b : s) {
            dc |= std::fabs(b.mhz - 5700) < 0.2;
            edge |= b.mhz < 5690 || b.mhz > 5710;
        }
        check(!dc && !edge && !s.empty(), "sin bins junto a DC ni fuera de [-10, +10) MHz");
    }
    // pasos a 20 MHz: cada bin lo mide un solo paso (sin solapes que lo cuenten dos veces por barrido)
    {
        auto a = capture(0, 0);
        const auto s1 = survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0);
        const auto s2 = survey_measure(a.data(), a.size(), 5720e6, kFs, kReach, 0);
        long max1 = -1, min2 = 1 << 30;
        for (const auto &b : s1.bins) max1 = std::max(max1, b.index);
        for (const auto &b : s2.bins) min2 = std::min(min2, b.index);
        check(max1 + 1 == min2, "pasos a 20 MHz: se tocan sin solaparse");
    }
    // saturacion: muestras a fondo de escala (un VTX al lado con demasiada ganancia)
    {
        auto a = capture(0, 0);
        const auto ok = survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0);
        auto b = capture(0, 0);
        for (auto &v : b) v = cfs(std::clamp(v.real() * 3000.f, -2047.f, 2047.f), std::clamp(v.imag() * 3000.f, -2047.f, 2047.f));
        const auto sat = survey_measure(b.data(), b.size(), 5700e6, kFs, kReach, 0);
        std::printf("  saturacion: limpio %.4f%% (pico %.3f), saturado %.1f%%\n", ok.clip * 100, ok.peak_abs, sat.clip * 100);
        check(ok.clip == 0 && ok.peak_abs < 0.01 && sat.clip > 0.1f, "detecta muestras a fondo de escala");
        auto c = ok;
        survey_scale(c, 10);
        check(std::fabs(c.floor / ok.floor - 10) < 1e-6 && std::fabs(c.bins[5].mean / ok.bins[5].mean - 10) < 1e-6,
              "survey_scale: +10 dB = x10 en potencia");
    }
    // memoria: con 5 barridos una rafaga que se fue se olvida antes que con 40
    {
        auto run = [&](int memory) {
            SurveyGrid g;
            g.reset();
            g.set_memory(memory);
            auto a = capture(0, 0);
            auto b = capture(std::sqrt(100.0), 0.3);
            g.merge(survey_measure(a.data(), a.size(), 5700e6, kFs, kReach, 0));
            for (int i = 0; i < memory; i++) {  // lleno de rafagas
                g.merge(survey_measure(b.data(), b.size(), 5720e6, kFs, kReach, g.reference(5720e6)));
                g.end_sweep();
            }
            for (int i = 0; i < 20; i++) {  // 20 barridos limpios
                g.merge(survey_measure(a.data(), a.size(), 5720e6, kFs, kReach, g.reference(5720e6)));
                g.end_sweep();
            }
            return level(g.spectrum(), 5712, 5728, &SurveyBin::mean_db);
        };
        const double m5 = run(5), m40 = run(40);
        std::printf("  20 barridos despues: memoria 5 -> %.1f dB, memoria 40 -> %.1f dB\n", m5, m40);
        check(m5 < 2 && m40 > 6, "Recordar: con 5 barridos olvida antes que con 40");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
