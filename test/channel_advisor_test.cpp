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
// suelo con ruido como el medido con el adaptador (USB 3, 54 barridos, 5480-5940 MHz): media -0.1..1.5 dB, picos
// 4-7 dB y duty ~2 %, salvo 5510-5640 MHz, donde los picos van por 7-9 dB en casi todo el tramo
std::vector<Bin> noisy() {
    std::vector<Bin> b;
    unsigned s = 12345;
    const auto u = [&s] {  // 0..1, determinista
        s = s * 1103515245u + 12345u;
        return static_cast<float>((s >> 8) & 0xffff) / 65535.f;
    };
    for (double f = 5300.078; f < 6000; f += 0.15625) {
        const float mean = -0.1f + 1.6f * u();
        const bool hot = f > 5510 && f < 5640;
        const float peak = hot ? 7.f + 2.f * u() : 4.f + 3.f * u();
        b.push_back(Bin{f, mean, peak, 0.01f + 0.03f * u()});
    }
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
    for (const auto &p : a.picks) s += p.r.ch.name + (p.pinned ? "* " : "") + (p.r.ch.hdzero ? "(hdz) " : " ");
    return s;
}
// la eleccion con el canal name (y sistema) y si va fijado
const Pick *find(const Advice &a, const std::string &name, bool hdzero = false) {
    for (const auto &p : a.picks)
        if (p.r.ch.name == name && p.r.ch.hdzero == hdzero) return &p;
    return nullptr;
}
int count(const Advice &a, const std::string &name, bool hdzero = false) {
    int n = 0;
    for (const auto &p : a.picks) n += p.r.ch.name == name && p.r.ch.hdzero == hdzero;
    return n;
}
int pinned(const Advice &a) {
    int n = 0;
    for (const auto &p : a.picks) n += p.pinned;
    return n;
}
bool has(const Pick &p, Note n) {
    for (auto x : p.notes)
        if (x == n) return true;
    return false;
}
// los no fijados, fuera de [lo, hi]
bool free_avoid(const Advice &a, double lo, double hi) {
    for (const auto &p : a.picks) {
        if (p.pinned) continue;
        const double half = width(p.r.ch) / 2;
        if (p.r.ch.mhz + half > lo && p.r.ch.mhz - half < hi) return false;
    }
    return true;
}
const Candidate kR1{"R1", 5658, false}, kR2{"R2", 5695, false}, kR3{"R3", 5732, false}, kR4{"R4", 5769, false},
    kR7{"R7", 5880, false}, kR8{"R8", 5917, false}, kF1{"F1", 5740, false}, kF4{"F4", 5800, false};
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
        // mi VTX analogico en R3 (5731.5) con su banda lateral a ~12 MHz: marcado como mio, R3 queda limpio
        auto b = flat();
        add_block(b, 5726.5, 5736.5, 34, 35, 1);
        add_block(b, 5740.8, 5746, 31, 32, 1);
        const Candidate r3{"R3", 5732, false}, r4{"R4", 5769, false}, r2{"R2", 5695, false};
        const float before = rate(r3, b, {}).noise_db;
        mask_signal(b, Signal{5731.5, 0, false, "R3"});
        const float after = rate(r3, b, {}).noise_db;
        std::printf("  mi VTX en R3: antes %.1f dB, quitado %.1f dB (R2 %.1f, R4 %.1f)\n", before, after,
                    rate(r2, b, {}).noise_db, rate(r4, b, {}).noise_db);
        check(before > 20 && after < 1.5 && rate(r2, b, {}).noise_db < 1.5 && rate(r4, b, {}).noise_db < 1.5,
              "marcar mi VTX quita su senal y sus bandas laterales, no la de otros");
        // otro piloto a 37 MHz (R4) sigue viendose
        auto c = flat();
        add_block(c, 5726.5, 5736.5, 34, 35, 1);
        add_block(c, 5764, 5774, 30, 31, 1);
        mask_signal(c, Signal{5731.5, 0, false, "R3"});
        check(rate(r4, c, {}).noise_db > 20, "un piloto ajeno al lado no se quita");
        // con un piloto ajeno pegado (5748, se solapan): sin decirle que esta ahi, el nivel de alrededor sale de su
        // senal y R3 queda tapado a ese nivel; diciendoselo (others), R3 queda al nivel del ruido
        auto d = flat();
        add_block(d, 5724, 5740, 25, 25, 1);
        add_block(d, 5740, 5756, 22, 22, 1);
        const Signal mine{5732, 0, false, "R3"}, near{5748, 0, false, ""};
        auto e = d;
        mask_signal(d, mine);
        mask_signal(e, mine, {mine, near});
        std::printf("  mi VTX en R3 con otro pegado en 5748: R3 %.1f dB sin others, %.1f dB con others\n",
                    rate(r3, d, {}).noise_db, rate(r3, e, {}).noise_db);
        check(rate(r3, e, {}).noise_db < 6, "el nivel de alrededor no sale de las otras senales (others)");
        mask_signal(e, near, {mine, near});
        check(rate(r3, e, {}).noise_db < 1.5, "y quitada tambien la ajena, R3 queda limpio");
        // quitando solo la mia, lo que esta en su faldon pero mas cerca del centro de la ajena (5744-5748) es de la
        // ajena: se queda (si no, un canal pegado a ella pareceria limpio)
        auto g = flat();
        add_block(g, 5724, 5740, 25, 25, 1);
        add_block(g, 5740, 5756, 22, 22, 1);
        mask_signal(g, mine, {mine, near});
        double lin = 0;
        int n = 0;
        for (const auto &x : g)
            if (x.mhz >= 5744 && x.mhz <= 5748) {
                lin += std::pow(10.0, x.mean_db / 10.0);
                n++;
            }
        const double side = n ? 10 * std::log10(lin / n) : 0;
        std::printf("  faldon de mi R3 junto a la ajena (5744-5748): %.1f dB\n", side);
        check(side > 20, "el faldon de la mia no borra el borde de una ajena pegada");
    }
    {
        // un AP sin trafico en el canal WiFi 153 (5765 MHz): solo beacons, pico 20 dB y encendido el 0.3 % del
        // tiempo. Por la parte del tiempo apenas cuesta; por estar ahi, si. Su bloque de 80 MHz (149-161,
        // 5735-5815) cubre R3-R5: ahi la mitad.
        auto b = flat();
        add_block(b, 5765 - 8.3, 5765 + 8.3, 0.6f, 20, 0.003f);
        const auto w = find_wifi(b);
        std::printf("  AP callado en 153: %zu red(es)", w.size());
        for (const auto &x : w) std::printf(" ch%d %.0f dB [%.0f-%.0f]", x.ch, x.peak_db, x.block_lo, x.block_hi);
        std::printf("\n");
        check(w.size() == 1 && w[0].ch == 153 && w[0].block_lo == 5735 && w[0].block_hi == 5815,
              "ve un WiFi sin trafico (beacons) en el canal 153");
        const Candidate r4{"R4", 5769, false}, r5{"R5", 5806, false}, r7{"R7", 5880, false};
        const auto c4 = rate(r4, b, {}, w), c5 = rate(r5, b, {}, w), c7 = rate(r7, b, {}, w);
        std::printf("  coste R4 %.1f (sin redes %.1f), R5 %.1f, R7 %.1f\n", c4.cost, rate(r4, b, {}).cost, c5.cost,
                    c7.cost);
        check(c4.wifi == 0 && !c4.wifi_block && c4.cost > rate(r4, b, {}).cost + 2,
              "encima del AP callado: cuesta aunque no transmita");
        check(c5.wifi == 0 && c5.wifi_block && c5.cost > c7.cost + 1 && c5.cost < c4.cost,
              "en el resto de su bloque de 80 MHz: menos, pero cuesta");
        auto a = advise(b, {}, analog("R"), {}, 4, 0);
        std::printf("  4 en R con el AP: %s\n", list(a).c_str());
        // sin las redes salia R1 R3 R6 R8 (banda limpia): R3 pisa el bloque del AP
        check(a.complete && a.wifi.size() == 1 && avoids(a, 5735, 5815),
              "4 en R: nadie encima del AP callado ni en su bloque de 80 MHz");
        auto a5 = advise(b, {}, analog("R"), {}, 5, 0);
        std::printf("  5 en R con el AP: %s\n", list(a5).c_str());
        check(a5.complete && avoids(a5, 5765 - 8.3, 5765 + 8.3), "5 en R: ninguno encima del AP callado");
    }
    {
        // el canal 132 (5660) partido entre dos pasos del barrido: solo se han visto beacons en su mitad de abajo
        auto b = flat();
        add_block(b, 5660 - 8.3, 5659.9, 0.6f, 18, 0.004f);
        const auto w = find_wifi(b);
        check(w.size() == 1 && w[0].ch == 132, "un WiFi visto solo en una mitad (dos pasos del barrido)");
    }
    {
        // una rafaga de 2 MHz en el centro del 149 no es un WiFi (no ocupa sus 16.6 MHz)
        auto b = flat();
        add_block(b, 5744, 5746, 0.6f, 25, 0.01f);
        check(find_wifi(b).empty(), "una rafaga estrecha no es un WiFi");
        // ni un piloto (constante: pico = media)
        auto c = flat();
        add_block(c, 5765 - 9, 5765 + 9, 25, 26, 1);
        check(find_wifi(c).empty(), "un piloto constante no es un WiFi");
        // un piloto que acaba de llegar (pico por delante de la media) encima de un canal WiFi: no se cuenta
        auto d = flat();
        add_block(d, 5765 - 9, 5765 + 9, 10, 25, 1);
        std::vector<Signal> seen{{5769, 0, false, "R4"}};
        check(advise(d, seen, analog("R"), {}, 2, 0).wifi.empty(), "un piloto recien llegado no cuenta como WiFi");
    }
    {
        // lo medido con el adaptador: ruido con picos de 4-9 dB y una red en el canal 100 (5491.7-5508.3 MHz, picos
        // 24-27 dB, hueco en el centro, encendida el 6 % del tiempo). Con el umbral fijo de 6 dB, el ruido de
        // 5510-5640 salia como los canales WiFi 104-128.
        auto b = noisy();
        add_block(b, 5491.7, 5508.3, 6.5f, 25, 0.06f);
        add_block(b, 5499.5, 5500.5, 2.9f, 17, 0.02f);
        const auto w = find_wifi(b);
        std::printf("  ruido real + red en el 100:");
        for (const auto &x : w) std::printf(" ch%d (%.0f dB)", x.ch, x.peak_db);
        std::printf("\n");
        check(w.size() == 1 && w[0].ch == 100, "con ruido real: solo la red del canal 100, no el ruido");
        check(w.size() == 1 && w[0].traffic, "la red del 100 tiene trafico (su media sube)");
        check(find_wifi(noisy()).empty(), "ruido real solo: ninguna red");
    }
    {
        // lo medido con el router en el canal 108 a 80 MHz y un video: todo el bloque 100-112 (5490-5570 MHz) a
        // 30-39 dB, encendido el 20-35 % del tiempo, y a la derecha sus hombros, de 24 dB en 5572 a 13 dB en 5596
        // (sin ellos, salian como redes en el 116 y el 120)
        auto b = noisy();
        add_block(b, 5490, 5570, 18, 33, 0.25f);
        for (double f = 5570; f < 5600; f += 2) {
            const float pk = 24 - static_cast<float>(f - 5570) * 0.37f;
            add_block(b, f, f + 2, 3.5f, pk, 0.04f);
        }
        const auto w = find_wifi(b);
        std::printf("  bloque de 80 MHz con trafico:");
        for (const auto &x : w) std::printf(" ch%d (%.0f dB)", x.ch, x.peak_db);
        std::printf("\n");
        bool block = w.size() == 4;
        for (size_t i = 0; block && i < w.size(); i++) block = w[i].ch == 100 + 4 * static_cast<int>(i);
        check(block, "un 80 MHz con trafico: los cuatro canales de su bloque, no sus hombros");
    }
    {
        // un AP callado se ve a ratos: cada paso mide 12 ms y hay un beacon cada ~102 ms (~12 % de pillarlo por
        // barrido), y el pico se olvida 1 dB por barrido (Recordar 20). Beacons a 20 dB pillados en los barridos 0,
        // 16 y 33: el espectro solo lo muestra mientras el pico no ha caido bajo el umbral; la memoria, siempre.
        const int caught[] = {0, 16, 33};
        float peak = 0;
        int raw = 0, remembered = 0;
        WifiMemory mem;
        for (int sw = 0; sw < 40; sw++) {
            peak -= 1;
            for (int c : caught)
                if (c == sw) peak = 20;
            auto b = flat();
            add_block(b, 5765 - 8.3, 5765 + 8.3, 0.6f, std::max(3.f, peak), 0.003f);
            const auto now = find_wifi(b);
            mem.update(now, {}, sw * 3.3);
            raw += !now.empty();
            remembered += !mem.known(sw * 3.3).empty();
        }
        std::printf("  AP callado, 40 barridos: visto en %d, recordado en %d\n", raw, remembered);
        check(raw <= 32 && remembered == 40, "un AP que se ve a ratos se recuerda todo el analisis");
        // y se olvida kWifiHoldS despues de la ultima vez
        mem.update({}, {}, 40 * 3.3 + kWifiHoldS);
        check(mem.known(40 * 3.3 + kWifiHoldS).empty(), "se olvida kWifiHoldS despues de verlo por ultima vez");
    }
    {
        // lo que la memoria cree WiFi y luego resulta ser un piloto (uno recien llegado parece intermitente)
        WifiMemory mem;
        Wifi fake;
        fake.ch = 153;
        fake.mhz = 5765;
        mem.update({fake}, {}, 0);
        mem.update({}, {{5769, 0, false, "R4"}}, 3);
        check(mem.known(3).empty(), "la memoria olvida un 'WiFi' que resulta ser un piloto");
        // lo que se recuerda sin verse ahora sale marcado
        mem.update({fake}, {}, 6);
        mem.update({}, {}, 9);
        const auto k = mem.known(9);
        check(k.size() == 1 && !k[0].now, "lo recordado que no se ve ahora: now = false");
    }
    {
        // el resto del bloque solo cuenta mientras no se sabe el ancho: con trafico, lo que usa se enciende solo
        // (medido: un AP a 20 MHz con trafico deja su bloque callado)
        auto idle = flat();
        add_block(idle, 5765 - 8.3, 5765 + 8.3, 0.6f, 20, 0.003f);
        auto busy = flat();
        add_block(busy, 5765 - 8.3, 5765 + 8.3, 6, 20, 0.1f);
        const Candidate r5{"R5", 5806, false};
        const auto wi = find_wifi(idle), wb = find_wifi(busy);
        check(wi.size() == 1 && !wi[0].traffic && wb.size() == 1 && wb[0].traffic, "trafico: por la media del canal");
        const auto ri = rate(r5, idle, {}, wi), rb = rate(r5, busy, {}, wb);
        std::printf("  R5 en el bloque del 153: callado %.1f dB, con trafico %.1f dB\n", ri.wifi_cost, rb.wifi_cost);
        check(ri.wifi_block && ri.wifi_cost > 0 && rb.wifi_cost == 0, "con trafico y el bloque callado: el bloque no cuenta");
    }
    {
        // bloques segun la region: en la UE no existen el 144 ni el 169-177
        auto b132 = flat();
        add_block(b132, 5660 - 8.3, 5660 + 8.3, 0.6f, 20, 0.003f);
        auto b165 = flat();
        add_block(b165, 5825 - 8.3, 5825 + 8.3, 0.6f, 20, 0.003f);
        const Candidate r2{"R2", 5695, false}, r7{"R7", 5880, false};
        const auto eu132 = find_wifi(b132, Region::Eu), us132 = find_wifi(b132, Region::Us);
        const auto eu165 = find_wifi(b165, Region::Eu), us165 = find_wifi(b165, Region::Us);
        std::printf("  bloque del 132: UE %.0f-%.0f, EE. UU. %.0f-%.0f; del 165: UE %.0f-%.0f, EE. UU. %.0f-%.0f\n",
                    eu132[0].block_lo, eu132[0].block_hi, us132[0].block_lo, us132[0].block_hi, eu165[0].block_lo,
                    eu165[0].block_hi, us165[0].block_lo, us165[0].block_hi);
        check(eu132[0].block_hi == 5690 && us132[0].block_hi == 5730, "132 a 40 MHz en la UE, a 80 en EE. UU.");
        check(rate(r2, b132, {}, eu132).wifi_cost < rate(r2, b132, {}, us132).wifi_cost,
              "R2 cuesta menos por un AP en el 132 en la UE");
        check(rate(r7, b165, {}, eu165).wifi_cost == 0 && rate(r7, b165, {}, us165).wifi_cost > 0,
              "un AP en el 165: en la UE va solo y no toca R7");
    }
    {
        // una red de 80 MHz es un grupo; dos bloques distintos, dos
        std::vector<Wifi> w;
        for (int ch : {100, 104, 108, 112, 132, 140}) {
            Wifi x;
            x.ch = ch;
            x.mhz = 5000 + 5.0 * ch;
            w.push_back(x);
        }
        for (auto &x : w) {
            double lo = 5000 + 5.0 * x.ch - 10, hi = lo + 20;
            if (x.ch <= 112) lo = 5490, hi = 5570;
            if (x.ch == 132) lo = 5650, hi = 5690;
            x.block_lo = lo;
            x.block_hi = hi;
        }
        const auto g = group_wifi(w);
        check(g.size() == 3 && g[0].first == 100 && g[0].last == 112 && g[1].first == 132 && g[2].first == 140,
              "agrupa los canales de una misma red (100-112) y separa bloques distintos");
    }
    // ---- canales fijados (advise, pinned): entran siempre y solo se buscan los que faltan ----
    {
        // sin fijar, 4 en R con la banda limpia sale R1 R3 R6 R8: fijando R2, la recomendacion cambia a su alrededor
        const auto base = advise(flat(), {}, analog("R"), {}, 4, 0);
        const auto a = advise(flat(), {}, analog("R"), {}, 4, 0, nullptr, {kR2});
        std::printf("  4 en R, R2 fijado: %s (sin fijar: %s)\n", list(a).c_str(), list(base).c_str());
        check(pinned(base) == 0 && !find(base, "R2"), "sin fijados: ningun Pick::pinned (y R2 no sale)");
        const Pick *r2 = find(a, "R2");
        check(r2 && r2->pinned && pinned(a) == 1, "fijado R2: sale, marcado como fijado, y solo el");
        check(a.complete && a.picks.size() == 4 && spaced(a), "fijado R2: busca los 3 que faltan, separados de R2");
    }
    {
        const auto a = advise(flat(), {}, analog("R"), {}, 4, 0, nullptr, {kR1, kR8});
        std::printf("  4 en R, R1 y R8 fijados: %s\n", list(a).c_str());
        check(a.complete && a.picks.size() == 4 && pinned(a) == 2 && find(a, "R1") && find(a, "R8") &&
                  find(a, "R1")->pinned && find(a, "R8")->pinned && spaced(a),
              "dos fijados (R1, R8) y dos buscados");
    }
    {
        // un fijado se respeta aunque este sucio (decision del usuario); los demas se apartan de lo sucio
        auto b = flat();
        add_block(b, 5650, 5690, 4, 30, 0.2f);  // WiFi a rafagas encima de R1
        const auto a = advise(b, {}, analog("R"), {}, 3, 0, nullptr, {kR1});
        std::printf("  R1 fijado bajo un WiFi: %s\n", list(a).c_str());
        check(a.picks.size() == 3 && find(a, "R1") && find(a, "R1")->pinned && free_avoid(a, 5650, 5690),
              "fijado en un canal sucio: se queda; los buscados evitan el WiFi");
    }
    {
        // un fijado donde ya vuela un piloto ajeno: se queda, con la nota de ocupado
        std::vector<Signal> seen{{5769, 0, false, "R4"}};
        auto b = flat();
        add_block(b, 5759, 5779, 20, 22, 1);
        const auto a = advise(b, seen, analog("R"), {}, 3, 0, nullptr, {kR4});
        const Pick *r4 = find(a, "R4");
        check(r4 && r4->pinned && has(*r4, Note::Busy) && a.picks.size() == 3,
              "fijado en un canal ocupado: se queda, con Note::Busy");
    }
    {
        // mas fijados que pilotos: salen todos los fijados y ninguno mas
        const auto a = advise(flat(), {}, analog("R"), {}, 2, 0, nullptr, {kR1, kR4, kR7});
        std::printf("  2 pilotos y 3 fijados: %s\n", list(a).c_str());
        check(a.picks.size() == 3 && pinned(a) == 3, "mas fijados que pilotos: todos los fijados, ninguno buscado");
        const auto all = advise(flat(), {}, analog("R"), {}, 3, 0, nullptr, {kR1, kR4, kR7});
        check(all.picks.size() == 3 && pinned(all) == 3 && all.complete, "tantos fijados como pilotos: esos mismos");
    }
    {
        // un fijado no sale dos veces aunque tambien sea candidato
        const auto a = advise(flat(), {}, analog("R"), {}, 4, 0, nullptr, {kR1});
        check(count(a, "R1") == 1 && a.picks.size() == 4, "un fijado que tambien es candidato no se repite");
    }
    {
        // un fijado fuera de las bandas elegidas (F4 con solo R): entra igual y R5 (6 MHz al lado) no puede salir
        const auto a = advise(flat(), {}, analog("R"), {}, 4, 0, nullptr, {kF4});
        std::printf("  F4 fijado, bandas R: %s\n", list(a).c_str());
        check(find(a, "F4") && find(a, "F4")->pinned && !find(a, "R5") && a.picks.size() == 4 && spaced(a),
              "un fijado fuera de las bandas elegidas tambien cuenta");
    }
    {
        // con fijados fuera de las bandas elegidas, el total puede pasar de los candidatos de esas bandas: solo L
        // (8 canales) y R1 y R8 fijados, 9 pilotos: los 2 fijados y 7 de L
        const auto a = advise(flat(), {}, analog("L"), {}, 9, 0, nullptr, {kR1, kR8});
        std::printf("  9 pilotos, bandas L, R1 y R8 fijados: %s\n", list(a).c_str());
        check(a.picks.size() == 9 && pinned(a) == 2 && a.complete && spaced(a),
              "fijados fuera de las bandas: busca los que faltan del total pedido (no menos)");
    }
    {
        // dos fijados demasiado juntos (R3 5732 y F1 5740): salen los dos, y se avisa de que no cabe
        const auto a = advise(flat(), {}, analog("R"), {}, 3, 0, nullptr, {kR3, kF1});
        check(find(a, "R3") && find(a, "F1") && !a.complete, "dos fijados por debajo de la separacion minima: !complete");
    }
    {
        // HDZero fijado (R3, 27 MHz) con 2 analogicos y 1 HDZero en total: el HDZero es el fijado y los analogicos
        // se apartan de el; un analogico "R3" no queda marcado como fijado (otro sistema)
        const Candidate h3{"R3", 5732, true, kHdzBw};
        const auto a = advise(flat(), {}, analog("R"), hdz(), 2, 1, nullptr, {h3});
        std::printf("  HDZero R3 fijado + 2 analogicos: %s\n", list(a).c_str());
        int h = 0;
        for (const auto &p : a.picks) h += p.r.ch.hdzero;
        check(a.complete && a.picks.size() == 3 && h == 1 && find(a, "R3", true) && find(a, "R3", true)->pinned &&
                  !find(a, "R3", false) && spaced(a),
              "HDZero fijado: cuenta como el HDZero, los analogicos separados");
    }
    {
        // fijar no deja la recomendacion peor de lo necesario: con R1 y R8 fijados en una banda limpia, los
        // buscados no quedan por debajo de la separacion ideal (37 MHz) si caben (R3-R6 caben a 74)
        const auto a = advise(flat(), {}, analog("R"), {}, 4, 0, nullptr, {kR1, kR8});
        bool notes = false;
        for (const auto &p : a.picks) notes = notes || has(p, Note::TooClose);
        check(!notes, "con sitio de sobra: los buscados respetan la separacion ideal con los fijados");
    }
    {
        // peor caso con fijados: no puede ser mas lento que sin ellos
        const Candidate h3{"R3", 5732, true, kHdzBw}, h6{"R6", 5843, true, kHdzBw};
        const auto t0 = std::chrono::steady_clock::now();
        auto a = advise(flat(), {}, analog("ABEFRL"), hdz(), 8, 8, nullptr, {kR1, kR8, h3, h6});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  8 + 8 en ABEFRL con 4 fijados: %.0f ms (%s)\n", ms, list(a).c_str());
        check(ms < 150 && pinned(a) == 4, "peor caso con fijados en menos de 150 ms");
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
