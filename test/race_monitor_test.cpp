// Seguimiento de carrera (src/race_monitor.cpp) con espectros sinteticos, sin adaptador.
//   race_monitor_test        todos los casos; sale con 1 si alguno falla
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "channel_advisor.h"
#include "race_monitor.h"

using namespace advisor;

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-66s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

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
// un piloto en el aire: su senal en el espectro y en la lista del barrido
void fly(std::vector<Bin> &b, std::vector<Signal> &now, double mhz, float snr, bool digital = false) {
    const double half = digital ? kHdzBw / 2 : 8;
    add_block(b, mhz - half, mhz + half, 25, 25, 1);
    Signal s;
    s.mhz = mhz;
    s.bw_mhz = digital ? kHdzBw : 0;
    s.digital = digital;
    s.snr_db = snr;
    now.push_back(s);
}

const Candidate R1{"R1", 5658, false}, R2{"R2", 5695, false}, R3{"R3", 5732, false}, R4{"R4", 5769, false},
    R5{"R5", 5806, false}, R6{"R6", 5843, false}, R7{"R7", 5880, false}, R8{"R8", 5917, false};
const std::vector<Candidate> kRace{R1, R3, R6, R8};  // reparto tipico de 4 en Raceband

std::vector<Candidate> raceband() { return {R1, R2, R3, R4, R5, R6, R7, R8}; }

// una carrera de n barridos; scene(sweep, bins, now, wifi) pone lo que hay en el aire en cada uno
using Scene = std::function<void(int, std::vector<Bin> &, std::vector<Signal> &, std::vector<Wifi> &)>;
void run(race::Monitor &m, int n, const Scene &scene) {
    for (int sw = 0; sw < n; sw++) {
        auto b = flat();
        std::vector<Signal> now;
        std::vector<Wifi> wifi;
        scene(sw, b, now, wifi);
        m.update(b, now, wifi);
    }
}
// los cuatro de kRace volando con esta SNR
void four(std::vector<Bin> &b, std::vector<Signal> &now, float snr = 25) {
    for (const auto &c : kRace) fly(b, now, c.mhz, snr);
}
int index_of(const race::Monitor &m, const std::string &name, bool hdzero = false) {
    for (size_t i = 0; i < m.health().size(); i++)
        if (m.health()[i].ch.name == name && m.health()[i].ch.hdzero == hdzero) return static_cast<int>(i);
    return -1;
}
// el canal nuevo guarda la separacion minima con los demas fijados y con estas senales ajenas (lo que tiene una
// ajena pegada a su canal no se ve si cae bajo la portadora de un fijado)
bool fits(const race::Monitor &m, const race::Move &mv, const std::vector<Signal> &foreign) {
    for (size_t i = 0; i < m.health().size(); i++) {
        if (static_cast<int>(i) == mv.from) continue;
        if (std::fabs(m.health()[i].ch.mhz - mv.to.mhz) < min_spacing(m.health()[i].ch, mv.to)) return false;
    }
    for (const auto &s : foreign) {
        const Candidate f{"", s.mhz, s.digital, s.digital ? s.bw_mhz : 0};
        if (std::fabs(s.mhz - mv.to.mhz) < min_spacing(f, mv.to)) return false;
    }
    return true;
}
void show(const race::Monitor &m) {
    for (const auto &h : m.health())
        std::printf("    %s%s: %d barridos, visto %d, cortes %d, SNR %.1f/%.1f, intrusos %d, canal %.1f, "
                    "penal %.1f, calidad %.1f\n",
                    h.ch.name.c_str(), h.ch.hdzero ? "(hdz)" : "", h.sweeps, h.seen, h.lost, h.snr_med, h.snr_low,
                    h.intruders, h.channel_cost, h.penalty, h.quality);
}
void show(const std::optional<race::Move> &mv, const race::Monitor &m) {
    if (!mv) {
        std::printf("    -> sin recomendacion\n");
        return;
    }
    std::printf("    -> mover %s a %s (+%.1f dB, %s)\n", m.health()[mv->from].ch.name.c_str(), mv->to.name.c_str(),
                mv->gain_db, mv->cause == race::Cause::Intruder ? "intruso" : "interferencia");
}
}  // namespace

int main() {
    {
        // carrera limpia: los cuatro se ven siempre, SNR 25; nada que mover
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int, auto &b, auto &now, auto &) { four(b, now); });
        std::printf("  carrera limpia:\n");
        show(m);
        bool ok = m.health().size() == 4 && m.sweeps() == 20;
        for (const auto &h : m.health())
            ok = ok && h.sweeps == 20 && h.seen == 20 && h.lost == 0 && h.intruders == 0 &&
                 std::fabs(h.snr_med - 25) < 0.5f && std::fabs(h.snr_low - 25) < 0.5f;
        check(ok, "carrera limpia: 20 barridos, todos vistos, SNR 25, sin cortes ni intrusos");
        bool clean = true;
        for (const auto &h : m.health()) clean = clean && h.channel_cost < 2 && h.quality > 20 && h.quality <= 25;
        check(clean, "su propia senal no cuenta como interferencia de su canal");
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(!mv, "carrera limpia: no recomienda mover a nadie");
    }
    {
        // su piloto un poco desviado (R3 en 5734): sigue siendo el suyo, no un intruso
        race::Monitor m;
        m.start(kRace);
        run(m, 12, [](int, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, 5734, 25);
            fly(b, now, 5843, 25);
            fly(b, now, 5917, 25);
        });
        const auto &h = m.health()[1];
        check(h.seen == 12 && h.intruders == 0 && h.channel_cost < 2, "un piloto 2 MHz desviado es el de su canal");
    }
    {
        // cortes: R3 no se ve en los barridos 5-8; R6 aterriza en el 15 (el final no cuenta); R8 sale en el 3 (el
        // principio no cuenta)
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int sw, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            if (sw < 5 || sw > 8) fly(b, now, 5732, 25);
            if (sw < 15) fly(b, now, 5843, 25);
            if (sw >= 3) fly(b, now, 5917, 25);
        });
        std::printf("  cortes:\n");
        show(m);
        const auto &h = m.health();
        check(h[1].seen == 16 && h[1].lost == 4, "R3: 4 barridos sin verlo entre medias = 4 cortes");
        check(h[2].seen == 15 && h[2].lost == 0, "R6: lo que falta al final (aterrizado) no son cortes");
        check(h[3].seen == 17 && h[3].lost == 0, "R8: lo que falta al principio (sin salir) no son cortes");
        check(m.worst() == 1, "el de peor video es el de los cortes (R3)");
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(!mv, "cortes con el canal limpio: no recomienda cambiar de canal");
    }
    {
        // SNR que va y viene: la mediana entre medias, el percentil 10 en lo peor
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int sw, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, 5732, 25);
            fly(b, now, 5843, sw % 2 ? 10.f : 25.f);
            fly(b, now, 5917, 25);
        });
        const auto &h = m.health()[2];
        check(h.snr_med >= 10 && h.snr_med <= 25 && h.snr_low <= 11 && h.snr_low >= 9,
              "SNR a ratos 10 y 25: mediana entre medias, percentil 10 en 10");
    }
    {
        // senal floja con el canal limpio: es el peor video, pero cambiar de canal no lo arregla
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, 5732, 25);
            fly(b, now, 5843, 25);
            fly(b, now, 5917, 6);
        });
        std::printf("  R8 flojo (SNR 6):\n");
        show(m);
        check(m.worst() == 3, "el de SNR baja es el de peor video");
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(!mv, "senal floja con el canal limpio: no recomienda mover");
    }
    {
        // un intruso: desde el barrido 4, alguien enciende un VTX en 5748 (16 MHz por encima de R3, se solapan)
        race::Monitor m;
        m.start(kRace);
        const Signal intruder{5748, 0, false, "", 15};
        const Scene scene = [&](int sw, auto &b, auto &now, auto &) {
            four(b, now);
            if (sw >= 4) {
                add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
                now.push_back(intruder);
            }
        };
        run(m, 9, scene);
        check(!m.suggest(raceband(), {}), "con menos de kMinSweeps barridos no recomienda nada");
        run(m, 11, [&](int sw, auto &b, auto &now, auto &w) { scene(sw + 9, b, now, w); });
        std::printf("  intruso en 5748:\n");
        show(m);
        const int r3 = index_of(m, "R3");
        check(m.health()[r3].intruders == 16, "R3: 16 barridos con la senal ajena encima");
        bool others = true;
        for (size_t i = 0; i < m.health().size(); i++)
            if (static_cast<int>(i) != r3) others = others && m.health()[i].intruders == 0;
        check(others, "a los demas no les cuenta como intruso");
        check(m.worst() == r3, "el del intruso es el de peor video");
        // el intruso cuenta una vez: en intruders, no tambien su energia en la interferencia del canal
        const auto &h3 = m.health()[r3];
        const float intr = race::kIntruderCost * 16 / 20;
        check(h3.channel_cost < 2 && std::fabs(h3.penalty - (h3.channel_cost + intr)) < 0.1f,
              "la energia del intruso no se suma tambien a la interferencia del canal");
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(mv && mv->from == r3 && mv->cause == race::Cause::Intruder, "recomienda mover a R3, por el intruso");
        check(mv && mv->to.name != "R3" && !mv->to.hdzero && fits(m, *mv, {intruder}),
              "a un canal analogico libre, separado de los demas fijados y del intruso");
        check(mv && mv->gain_db >= race::kMoveMinGainDb, "con una mejora de al menos kMoveMinGainDb");
        check(mv && mv->gain_db <= intr + 0.5f, "y la mejora no pasa de lo que quita el intruso (no cuenta doble)");
        // con todas las bandas hay canales pegados al suyo (A8 5725, B2 5752...): ninguno a menos de la separacion
        // minima del intruso, aunque bajo la portadora de R3 el espectro se vea limpio
        const auto all = m.suggest(analog_channels("ABEFR"), {});
        show(all, m);
        check(all && all->from == r3 && fits(m, *all, {intruder}), "con todas las bandas, tampoco pegado al intruso");
        // se cambia: lo suyo vuelve a empezar, lo de los demas sigue
        if (mv) {
            m.moved(mv->from, mv->to);
            const auto &h = m.health()[mv->from];
            check(h.ch.name == mv->to.name && h.sweeps == 0 && h.seen == 0 && h.intruders == 0 &&
                      m.health()[0].sweeps == 20 && m.sweeps() == 20,
                  "moved(): el movido empieza de cero, los demas conservan lo suyo");
        } else {
            check(false, "moved(): el movido empieza de cero, los demas conservan lo suyo");
        }
    }
    {
        // dos senales que casan con R3 (su piloto en 5732 y otro en 5735): la mas cercana es su piloto, la otra un
        // intruso
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int, auto &b, auto &now, auto &) {
            four(b, now);
            now.push_back(Signal{5735, 0, false, "", 12});
        });
        const auto &h = m.health()[1];
        std::printf("  dos senales en R3: visto %d, SNR %.1f, intrusos %d\n", h.seen, h.snr_med, h.intruders);
        check(h.seen == 20 && std::fabs(h.snr_med - 25) < 0.5f && h.intruders == 20,
              "dos senales en un fijado: la mas cercana es su piloto, la otra intruso");
    }
    {
        // un intruso intermitente en 5748: sale 2 barridos de cada 3 y en el ultimo no esta. Con todas las bandas,
        // el canal nuevo guarda igual la separacion con el (se recuerdan las ajenas de los ultimos barridos)
        race::Monitor m;
        m.start(kRace);
        const Signal intruder{5748, 0, false, "", 15};
        run(m, 20, [&](int sw, auto &b, auto &now, auto &) {
            four(b, now);
            if (sw % 3 != 1) {
                add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
                now.push_back(intruder);
            }
        });
        const auto mv = m.suggest(analog_channels("ABEFR"), {});
        std::printf("  intruso intermitente en 5748:\n");
        show(mv, m);
        check(mv && mv->from == 1 && fits(m, *mv, {intruder}),
              "intruso intermitente (no esta en el ultimo barrido): tampoco pegado a el");
    }
    {
        // tras mover a R3: su piloto sigue unos barridos en 5732 hasta que aterriza y cambia. Esa senal es la suya
        // (el canal que deja, leaving()), no una ajena: ni intruso de nadie ni su piloto en el canal nuevo
        race::Monitor m;
        m.start(kRace);
        const Signal intruder{5748, 0, false, "", 15};
        bool landed = false;
        double to_mhz = 0;
        const Scene scene = [&](int, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, landed ? to_mhz : 5732, 25);
            fly(b, now, 5843, 25);
            fly(b, now, 5917, 25);
            add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
            now.push_back(intruder);
        };
        run(m, 12, scene);
        const auto mv = m.suggest(raceband(), {});
        check(mv && mv->from == 1, "tras mover: hay recomendacion para R3");
        if (mv) {
            m.moved(mv->from, mv->to);
            to_mhz = mv->to.mhz;
            run(m, 5, scene);  // aun en 5732
            const auto &h = m.health()[1];
            bool none = true;
            for (size_t i = 0; i < m.health().size(); i++)
                if (i != 1) none = none && m.health()[i].intruders == 0;
            std::printf("  tras mover R3 a %s: dejando %zu canal(es), visto %d en el nuevo\n", mv->to.name.c_str(),
                        m.leaving().size(), h.seen);
            check(m.leaving().size() == 1 && m.leaving()[0].name == "R3" && h.seen == 0 && none,
                  "el canal que deja: su senal es suya (no intruso), no la de su canal nuevo");
            landed = true;
            run(m, 4, scene);  // ya en el nuevo
            check(m.leaving().empty() && m.health()[1].seen == 4,
                  "cuando ya no se le ve en el canal viejo, se olvida; en el nuevo se le ve");
        } else {
            check(false, "el canal que deja: su senal es suya (no intruso), no la de su canal nuevo");
            check(false, "cuando ya no se le ve en el canal viejo, se olvida; en el nuevo se le ve");
        }
    }
    {
        // la sugerencia no cambia de un barrido a otro por poco: R2 y R5 casi empatan (ruido de 3 dB que va de uno
        // a otro cada barrido). Sin la anterior cambia; pasandole la anterior (prev), se queda
        race::Monitor m;
        m.start(kRace);
        const Signal intruder{5748, 0, false, "", 15};
        int sweep = 0;
        const Scene scene = [&](int, auto &b, auto &now, auto &) {
            four(b, now);
            add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
            now.push_back(intruder);
            add_block(b, 5880 - 10, 5880 + 10, 8, 9, 1);  // R7 sucio: fuera
            if (sweep++ % 2) add_block(b, 5695 - 10, 5695 + 10, 3, 4, 1);
            else add_block(b, 5806 - 10, 5806 + 10, 3, 4, 1);
        };
        run(m, 12, scene);
        std::vector<std::string> bare, kept;
        std::optional<race::Move> prev;
        for (int k = 0; k < 6; k++) {
            run(m, 1, scene);
            const auto a = m.suggest(raceband(), {});
            const auto b = m.suggest(raceband(), {}, prev ? &*prev : nullptr);
            bare.push_back(a ? a->to.name : "-");
            kept.push_back(b ? b->to.name : "-");
            if (b) prev = b;
        }
        std::string sa, sb;
        for (const auto &x : bare) sa += x + " ";
        for (const auto &x : kept) sb += x + " ";
        std::printf("  casi empate R2/R5: sin prev %s| con prev %s\n", sa.c_str(), sb.c_str());
        bool flips = false, same = kept[0] != "-";
        for (size_t k = 1; k < bare.size(); k++) flips = flips || bare[k] != bare[k - 1];
        for (const auto &x : kept) same = same && x == kept[0];
        check(flips, "(sin prev, la sugerencia cambia entre R2 y R5)");
        check(same, "con la anterior, la sugerencia se mantiene");
    }
    {
        // dos con problemas a la vez: intruso en R3 (5748) y otro en R8 (5900). Una recomendacion por canal, y los
        // canales nuevos compatibles entre si (no los dos al mismo sitio), con los demas fijados y con los intrusos
        race::Monitor m;
        m.start(kRace);
        const Signal i3{5748, 0, false, "", 15}, i8{5900, 0, false, "", 15};
        run(m, 20, [&](int, auto &b, auto &now, auto &) {
            four(b, now);
            add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
            add_block(b, 5900 - 8, 5900 + 8, 22, 22, 1);
            now.push_back(i3);
            now.push_back(i8);
        });
        const auto all = m.suggest_all(analog_channels("ABEFR"), {});
        std::printf("  intrusos en R3 y R8:");
        for (const auto &mv : all)
            std::printf(" %s->%s (+%.0f)", m.health()[mv.from].ch.name.c_str(), mv.to.name.c_str(), mv.gain_db);
        std::printf("\n");
        bool both = all.size() == 2 && ((all[0].from == 1 && all[1].from == 3) || (all[0].from == 3 && all[1].from == 1));
        check(both, "dos canales con problemas: una recomendacion para cada uno");
        bool ok = both;
        if (both) {
            // el reparto despues de aplicar las dos: todos separados entre si y de los intrusos
            std::vector<Candidate> after;
            for (size_t i = 0; i < m.health().size(); i++) after.push_back(m.health()[i].ch);
            for (const auto &mv : all) after[mv.from] = mv.to;
            for (size_t a = 0; a < after.size(); a++)
                for (size_t b = a + 1; b < after.size(); b++)
                    ok = ok && std::fabs(after[a].mhz - after[b].mhz) >= min_spacing(after[a], after[b]);
            for (const auto &mv : all) ok = ok && fits(m, mv, {i3, i8});
        }
        check(ok, "los canales nuevos son compatibles entre si, con los demas y con los intrusos");
        const auto one = m.suggest(analog_channels("ABEFR"), {});
        check(one && both && one->from == all[0].from && one->to.name == all[0].to.name,
              "suggest() es la primera de suggest_all() (la del peor video)");
    }
    {
        // la misma energia encima de R3 sin que el barrido la detecte como senal: no es intruso, pero sigue
        // contando como interferencia de su canal (solo se quita la de las senales ajenas detectadas)
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int, auto &b, auto &now, auto &) {
            four(b, now);
            add_block(b, 5748 - 8, 5748 + 8, 22, 22, 1);
        });
        const auto &h = m.health()[1];
        check(h.intruders == 0 && h.channel_cost > 3, "energia ajena sin senal detectada: cuenta como interferencia");
    }
    {
        // una red WiFi con trafico encima de R2 (canal 140, 5700): la interferencia la da la red recordada (debajo
        // de la portadora del piloto no se ve)
        const std::vector<Candidate> pins{R2, R4, R6, R8};
        Wifi w140;
        w140.ch = 140;
        w140.mhz = 5700;
        w140.peak_db = 30;
        w140.mean_db = 8;
        w140.duty = 0.3f;
        w140.block_lo = 5690;
        w140.block_hi = 5710;
        w140.traffic = true;
        race::Monitor m;
        m.start(pins);
        run(m, 20, [&](int, auto &b, auto &now, auto &wifi) {
            add_block(b, 5700 - 8.3, 5700 + 8.3, 8, 30, 0.3f);
            for (const auto &c : pins) fly(b, now, c.mhz, 25);
            wifi.push_back(w140);
        });
        std::printf("  WiFi 140 encima de R2:\n");
        show(m);
        check(m.health()[0].channel_cost > m.health()[1].channel_cost + 3, "R2 bajo la red: su canal cuesta mas");
        check(m.worst() == 0, "R2 es el de peor video");
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(mv && mv->from == 0 && mv->cause == race::Cause::Interference && mv->gain_db >= race::kMoveMinGainDb,
              "recomienda mover a R2, por interferencia");
        check(mv && mv->to.name != "R2" && fits(m, *mv, {}) && std::fabs(mv->to.mhz - 5700) >= 10 + kWifiOccupied / 2,
              "a un canal fuera de la red y separado de los demas");
    }
    {
        // dos problemas: WiFi en R2 (poco) e intruso en R6 (mucho): primero el peor
        const std::vector<Candidate> pins{R2, R4, R6, R8};
        Wifi w140;
        w140.ch = 140;
        w140.mhz = 5700;
        w140.peak_db = 30;
        w140.mean_db = 8;
        w140.block_lo = 5690;
        w140.block_hi = 5710;
        w140.traffic = true;
        const Signal intruder{5859, 0, false, "", 15};
        race::Monitor m;
        m.start(pins);
        run(m, 20, [&](int, auto &b, auto &now, auto &wifi) {
            for (const auto &c : pins) fly(b, now, c.mhz, 25);
            add_block(b, 5859 - 8, 5859 + 8, 22, 22, 1);
            now.push_back(intruder);
            wifi.push_back(w140);
        });
        std::printf("  WiFi en R2 e intruso en R6:\n");
        show(m);
        const auto mv = m.suggest(raceband(), {});
        show(mv, m);
        check(mv && mv->from == 2 && mv->cause == race::Cause::Intruder && fits(m, *mv, {intruder}),
              "con dos problemas, primero el de peor video (R6, intruso)");
    }
    {
        // un fijado cuyo piloto no sale nunca: ni es el peor ni se recomienda moverlo
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, 5732, 25);
            fly(b, now, 5843, 20);
            add_block(b, 5917 - 8, 5917 + 8, 22, 22, 1);  // alguien ajeno encima de R8, que no ha salido
            now.push_back(Signal{5925, 0, false, "", 15});
        });
        check(m.health()[3].seen == 0 && m.worst() == 2, "un fijado que no sale no es el peor (el peor es R6)");
        const auto mv = m.suggest(raceband(), {});
        check(!mv || mv->from != 3, "ni se recomienda mover a uno que no ha salido");
    }
    {
        // HDZero fijado (R4, 27 MHz) y un analogico ajeno encima, casi en su centro: otro sistema = intruso
        const Candidate h4{"R4", 5769, true, kHdzBw};
        const std::vector<Candidate> pins{R1, h4, R8};
        const Signal intruder{5772, 0, false, "", 15};
        race::Monitor m;
        m.start(pins);
        run(m, 20, [&](int sw, auto &b, auto &now, auto &) {
            fly(b, now, 5658, 25);
            fly(b, now, 5770, 14, true);
            fly(b, now, 5917, 25);
            if (sw >= 2) now.push_back(intruder);  // su senal queda dentro de la del HDZero en el espectro
        });
        std::printf("  analogico ajeno sobre el HDZero R4:\n");
        show(m);
        const int hi = index_of(m, "R4", true);
        check(hi >= 0 && m.health()[hi].seen == 20 && m.health()[hi].intruders == 18,
              "el HDZero se reconoce como suyo; el analogico encima es intruso");
        std::vector<Candidate> hz;
        for (const auto &c : raceband()) hz.push_back(Candidate{c.name, c.mhz, true, kHdzBw});
        const auto mv = m.suggest(raceband(), hz);
        show(mv, m);
        check(mv && mv->from == hi && mv->to.hdzero && mv->to.name != "R4" && fits(m, *mv, {intruder}),
              "recomienda mover el HDZero a otro canal HDZero libre");
    }
    {
        // como en una carrera real: el espectro va cada ~3 s pero los pilotos solo se miden cada ~14 s. Entre medias
        // siguen en el aire (fresh = false, con la SNR vieja o 0): su portadora no es interferencia ni un corte
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [](int sw, auto &b, auto &now, auto &) {
            four(b, now, 25);
            for (auto &s : now) {
                s.fresh = sw % 5 == 0;
                if (!s.fresh) s.snr_db = 0;
            }
        });
        bool ok = true;
        for (const auto &h : m.health())
            ok = ok && h.seen == 20 && h.lost == 0 && h.channel_cost < 2 && h.snr_low >= 24;
        check(ok, "pilotos medidos 1 de cada 5 barridos: sin cortes, sin interferencia, SNR de las medidas");
        check(!m.suggest(raceband(), {}), "pilotos medidos 1 de cada 5 barridos: no recomienda mover a nadie");
    }
    {
        // un DJI (digital, 18 MHz) justo debajo de R1 sin pisarlo: su borde a 3 MHz del de R1 (5648). Medido en una
        // carrera real: lo que derramaba dentro de R1 quedaba tapado con la senal del propio piloto de R1 y no
        // contaba para nada
        const auto dji = [](double mhz) {
            Signal s;
            s.mhz = mhz;
            s.bw_mhz = 18;
            s.digital = true;
            s.snr_db = 15;
            return s;
        };
        race::Monitor m;
        m.start(kRace);
        run(m, 20, [&](int, auto &b, auto &now, auto &) {
            four(b, now);
            add_block(b, 5627, 5645, 15, 15, 1);
            now.push_back(dji(5636));
        });
        const int r1 = index_of(m, "R1");
        check(r1 >= 0 && m.health()[r1].intruders == 20, "DJI a 3 MHz del borde de R1: cuenta como intruso de R1");
        bool others = true;
        for (size_t i = 0; i < m.health().size(); i++)
            if (static_cast<int>(i) != r1) others = others && m.health()[i].intruders == 0;
        check(others, "DJI junto a R1: a los demas no les cuenta");

        race::Monitor far;
        far.start(kRace);
        run(far, 20, [&](int, auto &b, auto &now, auto &) {
            four(b, now);
            add_block(b, 5617, 5635, 15, 15, 1);
            now.push_back(dji(5626));
        });
        check(far.health()[index_of(far, "R1")].intruders == 0, "DJI a 13 MHz del borde de R1: no es intruso");

        race::Monitor an;
        an.start(kRace);
        run(an, 20, [&](int, auto &b, auto &now, auto &) {
            four(b, now);
            fly(b, now, 5625, 15);  // analogico a 3 MHz del borde: solo cuenta si lo pisa (como antes)
        });
        check(an.health()[index_of(an, "R1")].intruders == 0, "analogico a 3 MHz del borde de R1: no es intruso");
    }
    {
        // 8 fijados (toda Raceband) con todas las bandas como alternativa: update y suggest caben en el segundo
        // que hay entre barridos (hilo de la GUI)
        race::Monitor m;
        m.start(raceband());
        const auto t0 = std::chrono::steady_clock::now();
        run(m, 20, [](int, auto &b, auto &now, auto &) {
            for (const auto &c : raceband()) fly(b, now, c.mhz, 25);
            now.push_back(Signal{5748, 0, false, "", 15});
        });
        const double up = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        const auto t1 = std::chrono::steady_clock::now();
        const auto mv = m.suggest(analog_channels("ABEFRL"), hdzero_channels("ABEFRL"));
        const double sg = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        std::printf("  8 fijados: update %.1f ms/barrido, suggest %.0f ms\n", up, sg);
        check(up < 50 && sg < 150, "8 fijados: update < 50 ms, suggest < 150 ms");
        (void)mv;
    }
    {
        // auto-inicio: cuantos fijados tienen a su piloto en el aire
        using advisor::Signal;
        const std::vector<Signal> none;
        check(race::present(kRace, none) == 0, "present: nadie en el aire = 0");
        std::vector<Signal> two{Signal{kRace[0].mhz + 2, 0, false, "", 20}, Signal{kRace[2].mhz, 0, false, "", 20}};
        check(race::present(kRace, two) == 2, "present: dos pilotos (uno desviado 2 MHz) = 2");
        two.push_back(Signal{kRace[0].mhz - 1, 0, false, "", 20});  // otro en el mismo canal: no suma
        check(race::present(kRace, two) == 2, "present: dos senales en el mismo canal cuentan una vez");
        two.push_back(Signal{kRace[1].mhz, 13.5, true, "", 20});  // un digital en un canal analogico: no es el suyo
        two.push_back(Signal{kRace[3].mhz + 20, 0, false, "", 20});  // lejos de todos
        check(race::present(kRace, two) == 2, "present: otro sistema o lejos de los fijados no cuenta");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
