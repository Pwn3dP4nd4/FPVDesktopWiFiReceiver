// Recomendador de canales (src/channel_advisor.cpp) con espectros sinteticos, sin adaptador.
//   channel_advisor_test        todos los casos; sale con 1 si alguno falla
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "channel_advisor.h"
#include "spectrum.h"  // kp::fpv_channels()

using namespace advisor;

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// suelo plano 5300..6000 MHz cada 156.25 kHz
std::vector<Bin> flat() {
    std::vector<Bin> b;
    for (double f = 5300.078; f < 6000; f += 0.15625) b.push_back(Bin{f, 0.5f, 3.f, 0.f});
    return b;
}
void add_block(std::vector<Bin> &b, double lo, double hi, float mean, float peak, float duty) {
    for (auto &x : b)
        if (x.mhz >= lo && x.mhz <= hi) {
            x.mean_db = mean;
            x.peak_db = peak;
            x.duty = duty;
        }
}
std::vector<Candidate> analog(const std::string &bands) {
    std::vector<Candidate> c;
    for (const auto &ch : kp::fpv_channels())
        if (bands.find(ch.name[0]) != std::string::npos) c.push_back(Candidate{ch.name, ch.mhz, false});
    return c;
}
std::vector<Candidate> hdz() {
    const char *n[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8"};
    const double f[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    std::vector<Candidate> c;
    for (int i = 0; i < 8; i++) c.push_back(Candidate{n[i], f[i], true, kHdzBw});
    return c;
}
bool spaced(const Advice &a) {
    for (size_t i = 0; i < a.picks.size(); i++)
        for (size_t j = i + 1; j < a.picks.size(); j++)
            if (std::fabs(a.picks[i].r.ch.mhz - a.picks[j].r.ch.mhz) <
                min_spacing(a.picks[i].r.ch, a.picks[j].r.ch))
                return false;
    return true;
}
bool avoids(const Advice &a, double lo, double hi) {
    for (const auto &p : a.picks) {
        const double half = width(p.r.ch) / 2;
        if (p.r.ch.mhz + half > lo && p.r.ch.mhz - half < hi) return false;
    }
    return true;
}
std::string list(const Advice &a) {
    std::string s;
    for (const auto &p : a.picks) s += p.r.ch.name + (p.r.ch.hdzero ? "(hdz) " : " ");
    return s;
}
}  // namespace

int main() {
    {
        auto a = advise(flat(), {}, analog("R"), {}, 4, 0);
        std::printf("  limpio, 4 en R: %s\n", list(a).c_str());
        check(a.complete && a.picks.size() == 4 && spaced(a), "banda limpia: 4 analogicos en R, separados");
    }
    {
        auto b = flat();
        add_block(b, 5650, 5690, 4, 30, 0.2f);  // WiFi canal 136 (20 MHz) y vecino: rafagas
        auto a = advise(b, {}, analog("R"), {}, 3, 0);
        std::printf("  WiFi 5650-5690: %s\n", list(a).c_str());
        check(a.complete && avoids(a, 5650, 5690), "evita un WiFi a rafagas (5650-5690 MHz)");
    }
    {
        std::vector<Signal> seen{{5769, 0, false, "R4"}};
        auto b = flat();
        add_block(b, 5759, 5779, 20, 22, 1);
        auto a = advise(b, seen, analog("R"), {}, 4, 0);
        std::printf("  R4 ocupado: %s\n", list(a).c_str());
        check(a.complete && avoids(a, 5759, 5779), "no elige un canal donde ya vuela un piloto (R4)");
    }
    {
        auto a = advise(flat(), {}, analog("R"), hdz(), 2, 2);
        std::printf("  2 + 2 HDZero: %s\n", list(a).c_str());
        int h = 0;
        for (const auto &p : a.picks) h += p.r.ch.hdzero;
        check(a.complete && a.picks.size() == 4 && h == 2 && spaced(a), "2 analogicos + 2 HDZero sin pisarse");
    }
    {
        std::vector<Signal> seen{{5732, 27, true, "R3"}};
        auto b = flat();
        add_block(b, 5718.5, 5745.5, 25, 27, 1);
        auto a = advise(b, seen, analog("R"), hdz(), 2, 1);
        std::printf("  HDZero en R3: %s\n", list(a).c_str());
        check(a.complete && avoids(a, 5718.5, 5745.5), "respeta un HDZero en el aire (R3, 27 MHz)");
    }
    {
        auto a = advise(flat(), {}, analog("R"), hdz(), 6, 3);
        std::printf("  demasiados (6 + 3 en 5.65-5.92 GHz): %s\n", list(a).c_str());
        check(!a.complete, "avisa cuando no caben (6 analogicos + 3 HDZero)");
    }
    {
        auto a = advise(flat(), {}, analog("ABEFR"), {}, 8, 0);
        std::printf("  8 en ABEFR: %s\n", list(a).c_str());
        check(a.complete && a.picks.size() == 8 && spaced(a), "8 analogicos con todas las bandas");
    }
    {
        // el peor caso de la ventana (todas las bandas, 8 + 8): se recalcula en el hilo de la GUI cada segundo
        const auto t0 = std::chrono::steady_clock::now();
        auto a = advise(flat(), {}, analog("ABEFRL"), hdz(), 8, 8);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  8 + 8 en ABEFRL: %.0f ms (%s)\n", ms, list(a).c_str());
        check(ms < 150, "peor caso en menos de 150 ms");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
