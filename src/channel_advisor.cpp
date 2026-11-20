#include "channel_advisor.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include "spectrum.h"  // kp::fpv_channels()

namespace advisor {

namespace {
constexpr float kBusyCost = 60;        // ya hay un piloto: solo si no queda otro remedio
constexpr float kUnknownCost = 20;     // menos de la mitad del canal analizado
constexpr float kBurstWeight = 20;     // dB si lo pisan rafagas todo el tiempo (30 % del tiempo: 6 dB)
constexpr double kSpacingCost = 1.0;   // dB por MHz por debajo de la separacion ideal
constexpr double kSpreadCost = 40;     // dB*MHz: 1 dB a 40 MHz, 0.5 dB a 80 MHz (separar mas, si se puede)
constexpr float kImdCost = 3;          // dB por producto de intermodulacion encima de otro canal
constexpr double kImdReach = 4;        // MHz: un producto a menos de esto de un canal analogico lo afecta
constexpr float kInfeasible = 1000;    // separacion por debajo de la minima
// Una red WiFi suma por estar ahi aunque ahora calle: 0.25 dB por dB de pico sobre kWifiCostFrom, hasta 6 dB (un
// beacon a 20 dB: 3.5 dB, como un canal pisado el 17 % del tiempo). En el resto de su bloque, la mitad.
constexpr float kWifiCostFrom = 6, kWifiCostPerDb = 0.25f, kWifiMaxCost = 6, kWifiBlockShare = 0.5f;
constexpr double kWifiLitShare = 0.6;  // parte de los bins de una mitad del canal que tienen que haberse encendido
constexpr double kBinMhz = 0.15625;    // ancho de un bin del barrido (fpv::SurveyGrid::kBin)
// un canal WiFi detectado con otro a menos de esto (MHz entre centros) al menos tantos dB mas fuerte es su hombro
constexpr double kWifiShoulderMhz = 40;
constexpr float kWifiShoulderDb = 10;

// Canales WiFi de 20 MHz que se buscan (de 4 en 4): 68-96 no existen ni en la UE ni en EE. UU. (hueco 5350-5470)
struct ChRange {
    int first, last;
};
constexpr ChRange kWifiChannels[] = {{36, 64}, {100, 144}, {149, 177}};
// El bloque mas ancho que puede usar cada canal (primero y ultimo de cada uno), segun la region; un canal fuera de
// todos va solo (20 MHz)
constexpr ChRange kBondsEu[] = {{36, 48}, {52, 64}, {100, 112}, {116, 128}, {132, 136}, {149, 161}};
constexpr ChRange kBondsUs[] = {{36, 48}, {52, 64}, {100, 112}, {116, 128}, {132, 144}, {149, 161}, {165, 177}};

void wifi_block(int ch, Region region, double &lo, double &hi) {
    lo = 5000 + 5.0 * ch - 10;
    hi = lo + 20;
    const auto pick = [&](const ChRange *b, size_t n) {
        for (size_t i = 0; i < n; i++)
            if (ch >= b[i].first && ch <= b[i].last) {
                lo = 5000 + 5.0 * b[i].first - 10;
                hi = 5000 + 5.0 * b[i].last + 10;
            }
    };
    if (region == Region::Eu) pick(kBondsEu, std::size(kBondsEu));
    else pick(kBondsUs, std::size(kBondsUs));
}

double sig_width(const Signal &s) { return s.digital && s.bw_mhz > 0 ? s.bw_mhz : kAnalogBw; }

bool covered_by_pilot(const Wifi &w, const std::vector<Signal> &seen) {
    for (const auto &s : seen)
        if (std::fabs(w.mhz - s.mhz) < sig_width(s) / 2) return true;
    return false;
}

float median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

float pair_cost(const Candidate &a, const Candidate &b, bool *close) {
    const double d = std::fabs(a.mhz - b.mhz);
    if (d < min_spacing(a, b)) return kInfeasible;
    const double ideal = ideal_spacing(a, b);
    // a little for every pair, less the further apart: with a clean band the pilots spread out
    const float spread = static_cast<float>(kSpreadCost / d);
    if (d >= ideal) return spread;
    if (close) *close = true;
    return spread + static_cast<float>((ideal - d) * kSpacingCost);
}

// 2*f1 - f2 de cada par de analogicos elegidos sobre otro canal elegido c (analogico: cerca de la portadora;
// HDZero: dentro de su ancho). Solo pares analogicos: una portadora FM concentra el producto en pocos MHz; el de
// una senal digital se reparte como ruido sobre tres veces su ancho y, en la rejilla de 37 MHz, saldria en casi
// cualquier combinacion sin decir nada util.
int imd_hits(const std::vector<const Candidate *> &set, const Candidate &c) {
    const double reach = c.hdzero ? width(c) / 2 : kImdReach;
    int hits = 0;
    for (const auto *a : set)
        for (const auto *b : set) {
            if (a == b || a == &c || b == &c || a->hdzero || b->hdzero) continue;
            if (std::fabs(2 * a->mhz - b->mhz - c.mhz) < reach) hits++;
        }
    return hits;
}

struct Search {
    const std::vector<Rating> &all;  // analogicos y HDZero juntos
    int need_a, need_h;              // total de cada sistema, fijados incluidos
    const std::vector<int> &fixed;   // indices en all de los fijados: van siempre al principio de cada eleccion

    float total(const std::vector<int> &pick) const {
        float t = 0;
        for (size_t i = 0; i < pick.size(); i++) {
            t += all[pick[i]].cost;
            for (size_t j = i + 1; j < pick.size(); j++) t += pair_cost(all[pick[i]].ch, all[pick[j]].ch, nullptr);
        }
        std::vector<const Candidate *> set;
        for (int i : pick) set.push_back(&all[i].ch);
        for (const auto *c : set) t += kImdCost * static_cast<float>(imd_hits(set, *c));
        return t;
    }

    bool fits(const std::vector<int> &pick, int i) const {
        if (std::find(pick.begin(), pick.end(), i) != pick.end()) return false;
        int a = 0, h = 0;
        for (int p : pick) (all[p].ch.hdzero ? h : a)++;
        return all[i].ch.hdzero ? h < need_h : a < need_a;
    }

    // desde los fijados mas start (-1: solo los fijados)
    std::vector<int> greedy(int start) const {
        std::vector<int> pick = fixed;
        if (start >= 0) pick.push_back(start);
        while (static_cast<int>(pick.size()) < need_a + need_h) {
            int best = -1;
            float bt = std::numeric_limits<float>::max();
            for (int i = 0; i < static_cast<int>(all.size()); i++) {
                if (!fits(pick, i)) continue;
                auto p = pick;
                p.push_back(i);
                const float t = total(p);
                if (t < bt) {
                    bt = t;
                    best = i;
                }
            }
            if (best < 0) break;
            pick.push_back(best);
        }
        return pick;
    }

    void improve(std::vector<int> &pick) const {
        float cur = total(pick);
        for (int round = 0; round < 50; round++) {
            bool better = false;
            for (size_t k = fixed.size(); k < pick.size(); k++)  // un fijado no se cambia nunca
                for (int i = 0; i < static_cast<int>(all.size()); i++) {
                    if (all[i].ch.hdzero != all[pick[k]].ch.hdzero) continue;
                    if (std::find(pick.begin(), pick.end(), i) != pick.end()) continue;
                    auto p = pick;
                    p[k] = i;
                    const float t = total(p);
                    if (t < cur - 1e-3f) {
                        cur = t;
                        pick = p;
                        better = true;
                    }
                }
            if (!better) break;
        }
    }
};
}  // namespace

std::vector<Candidate> analog_channels(const std::string &bands) {
    std::vector<Candidate> out;
    for (const auto &c : kp::fpv_channels())
        if (bands.find(c.name[0]) != std::string::npos) out.push_back(Candidate{c.name, c.mhz, false});
    return out;
}

std::vector<Candidate> hdzero_channels(const std::string &bands, bool narrow) {
    static const char *kNames[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "E1", "F1", "F2", "F4",
                                   "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8"};
    std::vector<Candidate> out;
    for (const char *n : kNames) {
        if (bands.find(n[0]) == std::string::npos) continue;
        for (const auto &c : kp::fpv_channels())
            if (std::string(c.name) == n) out.push_back(Candidate{c.name, c.mhz, true, narrow ? kHdzNarrowBw : kHdzBw});
    }
    return out;
}

double width(const Candidate &c) { return c.bw_mhz > 0 ? c.bw_mhz : c.hdzero ? kHdzBw : kAnalogBw; }

double min_spacing(const Candidate &a, const Candidate &b) {
    // dos analogicos FM a 20 MHz se pisan (armonicos, desviacion) a poca distancia: 25 como minimo
    if (!a.hdzero && !b.hdzero) return 25;
    return (width(a) + width(b)) / 2;  // los bordes se tocan
}

double ideal_spacing(const Candidate &a, const Candidate &b) {
    if (a.hdzero && b.hdzero) return 37;  // la rejilla de canales de HDZero
    if (a.hdzero || b.hdzero) return 40;
    return 37;                            // Raceband
}

void mask_signal(std::vector<Bin> &bins, const Signal &s, const std::vector<Signal> &others) {
    const double half = sig_width(s) / 2;
    std::vector<float> ring, all;
    for (const auto &b : bins) {
        const double d = std::fabs(b.mhz - s.mhz);
        if (d <= half + kMaskSkirt || d > half + kMaskSkirt + kMaskRing) continue;
        all.push_back(b.mean_db);
        bool covered = false;
        for (const auto &o : others)
            covered = covered || std::fabs(b.mhz - o.mhz) <= sig_width(o) / 2 + kMaskSkirt;
        if (!covered) ring.push_back(b.mean_db);
    }
    if (ring.empty()) ring = std::move(all);
    float level = 0;
    if (!ring.empty()) {
        std::nth_element(ring.begin(), ring.begin() + ring.size() / 2, ring.end());
        level = ring[ring.size() / 2];
    }
    for (auto &b : bins) {
        const double d = std::fabs(b.mhz - s.mhz);
        if (d <= half || (d <= half + kMaskSkirt && b.mean_db > level + 3)) b = Bin{b.mhz, level, level, 0};
    }
}

std::vector<Wifi> find_wifi(const std::vector<Bin> &bins, Region region, const std::vector<Signal> &seen) {
    std::vector<Wifi> out;
    if (bins.empty()) return out;
    const double half = kWifiOccupied / 2;
    const double expect = half / kBinMhz;  // bins en una mitad
    float lit_at;                          // pico que tiene que pasar un bin (dB sobre el suelo)
    {
        // percentil 25 de los picos: el ruido aunque haya mucho ocupado (WiFi, pilotos) en la banda
        std::vector<float> pk;
        pk.reserve(bins.size());
        for (const auto &b : bins) pk.push_back(b.peak_db);
        std::nth_element(pk.begin(), pk.begin() + pk.size() / 4, pk.end());
        lit_at = std::max(kWifiPeakMin, pk[pk.size() / 4] + kWifiOverNoise);
    }
    for (const auto &g : kWifiChannels)
        for (int n = g.first; n <= g.last; n += 4) {
            const double c = 5000 + 5.0 * n;
            int total[2] = {0, 0}, lit[2] = {0, 0};
            double duty = 0;
            std::vector<float> peaks, means;
            for (const auto &b : bins) {
                const double d = b.mhz - c;
                if (std::fabs(d) > half) continue;
                const int side = d < 0 ? 0 : 1;
                total[side]++;
                duty += b.duty;
                means.push_back(b.mean_db);
                if (b.peak_db - b.mean_db > kBurstPeakOverMean && b.peak_db >= lit_at) {
                    lit[side]++;
                    peaks.push_back(b.peak_db);
                }
            }
            bool on = false;
            for (int s = 0; s < 2; s++) on = on || (total[s] >= expect / 2 && lit[s] >= kWifiLitShare * total[s]);
            if (!on) continue;
            Wifi w;
            w.ch = n;
            w.mhz = c;
            w.peak_db = median(peaks);
            w.mean_db = median(means);
            w.duty = static_cast<float>(duty / (total[0] + total[1]));
            w.traffic = w.mean_db >= kWifiTrafficDb;
            wifi_block(n, region, w.block_lo, w.block_hi);
            if (!covered_by_pilot(w, seen)) out.push_back(w);
        }
    // los hombros de una senal fuerte (lo que derrama un WiFi a los lados: medido, un bloque de 80 MHz a 33 dB baja
    // de 24 a 13 dB en los 30 MHz siguientes) parecen canales WiFi flojos al lado: fuera los que tienen cerca otro
    // mucho mas fuerte
    std::vector<Wifi> kept;
    for (const auto &w : out) {
        bool shoulder = false;
        for (const auto &o : out)
            shoulder = shoulder || (std::fabs(o.mhz - w.mhz) <= kWifiShoulderMhz && o.peak_db >= w.peak_db + kWifiShoulderDb);
        if (!shoulder) kept.push_back(w);
    }
    return kept;
}

void WifiMemory::update(const std::vector<Wifi> &now, const std::vector<Signal> &seen, double t_s) {
    // lo que tapa un piloto no es un WiFi (uno recien llegado lo parece unos barridos): se olvida
    nets_.erase(std::remove_if(nets_.begin(), nets_.end(), [&](const Entry &e) { return covered_by_pilot(e.w, seen); }),
                nets_.end());
    for (const auto &w : now) {
        if (covered_by_pilot(w, seen)) continue;
        auto it = std::find_if(nets_.begin(), nets_.end(), [&](const Entry &e) { return e.w.ch == w.ch; });
        if (it == nets_.end()) {
            nets_.push_back(Entry{w, t_s});
            continue;
        }
        Wifi &k = it->w;
        k.peak_db = std::max(k.peak_db, w.peak_db);
        k.mean_db = std::max(k.mean_db, w.mean_db);
        k.duty = std::max(k.duty, w.duty);
        k.traffic = k.traffic || w.traffic;
        k.block_lo = w.block_lo;
        k.block_hi = w.block_hi;
        it->last_s = t_s;
    }
    last_update_s_ = t_s;
}

std::vector<Wifi> WifiMemory::known(double t_s) const {
    std::vector<Wifi> out;
    for (const auto &e : nets_) {
        if (t_s - e.last_s > kWifiHoldS) continue;
        Wifi w = e.w;
        w.now = e.last_s >= last_update_s_;
        out.push_back(w);
    }
    std::sort(out.begin(), out.end(), [](const Wifi &a, const Wifi &b) { return a.ch < b.ch; });
    return out;
}

std::vector<WifiGroup> group_wifi(const std::vector<Wifi> &wifi) {
    auto w = wifi;
    std::sort(w.begin(), w.end(), [](const Wifi &a, const Wifi &b) { return a.ch < b.ch; });
    std::vector<WifiGroup> out;
    for (size_t i = 0; i < w.size(); i++) {
        if (!out.empty() && w[i].ch == out.back().last + 4 && w[i].block_lo == w[i - 1].block_lo &&
            w[i].block_hi == w[i - 1].block_hi) {
            out.back().last = w[i].ch;
            out.back().hi = w[i].mhz + kWifiOccupied / 2;
            continue;
        }
        out.push_back(WifiGroup{w[i].ch, w[i].ch, w[i].mhz - kWifiOccupied / 2, w[i].mhz + kWifiOccupied / 2});
    }
    return out;
}

Rating rate(const Candidate &c, const std::vector<Bin> &bins, const std::vector<Signal> &seen,
            const std::vector<Wifi> &wifi) {
    Rating r;
    r.ch = c;
    const double half = width(c) / 2;
    double lin = 0, burst = 0;
    int n = 0;
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (const auto &b : bins) {
        if (std::fabs(b.mhz - c.mhz) > half) continue;
        lin += std::pow(10.0, b.mean_db / 10.0);
        // intermitente: su pico reciente esta muy por encima de su media; cuenta el tiempo que esta encendido
        if (b.peak_db - b.mean_db > kBurstPeakOverMean) burst += b.duty;
        lo = std::min(lo, b.mhz);
        hi = std::max(hi, b.mhz);
        n++;
    }
    r.coverage = n ? static_cast<float>(std::min(1.0, (hi - lo + 0.15625) / (2 * half))) : 0.f;
    r.noise_db = n ? static_cast<float>(10 * std::log10(lin / n)) : 0.f;
    r.burst = n ? static_cast<float>(burst / n) : 0.f;
    // redes WiFi: la que mas pese, encima (por la parte del canal que pisa) o en el resto de su bloque de 80 MHz
    const auto overlap = [&](double a, double b) {
        return std::max(0.0, std::min(b, c.mhz + half) - std::max(a, c.mhz - half));
    };
    for (size_t i = 0; i < wifi.size(); i++) {
        const Wifi &w = wifi[i];
        const float presence = std::clamp((w.peak_db - kWifiCostFrom) * kWifiCostPerDb, 0.f, kWifiMaxCost);
        const double on = overlap(w.mhz - kWifiOccupied / 2, w.mhz + kWifiOccupied / 2);
        float cost;
        bool block = false;
        if (on > 0) {
            cost = presence * static_cast<float>(std::min(1.0, on / std::min(2 * half, kWifiOccupied)));
        } else {
            // el resto de su bloque, solo si no se sabe su ancho: con trafico, lo que usa ya se enciende y se
            // detecta (medido: un 20 MHz con trafico deja su bloque callado)
            if (w.traffic) continue;
            const double in = overlap(w.block_lo, w.block_hi);
            if (in <= 0) continue;
            cost = kWifiBlockShare * presence * static_cast<float>(std::min(1.0, in / (2 * half)));
            block = true;
        }
        if (cost > r.wifi_cost) {
            r.wifi_cost = cost;
            r.wifi = static_cast<int>(i);
            r.wifi_block = block;
        }
    }
    r.cost = r.noise_db + kBurstWeight * r.burst + r.wifi_cost;
    if (r.coverage < 0.5f) r.cost += kUnknownCost;
    for (size_t i = 0; i < seen.size(); i++) {
        const double gap = std::fabs(seen[i].mhz - c.mhz) - (sig_width(seen[i]) + width(c)) / 2;
        if (gap < 0) {
            r.busy = static_cast<int>(i);
            r.cost += kBusyCost;
            break;
        }
    }
    return r;
}

Advice advise(const std::vector<Bin> &bins, const std::vector<Signal> &seen, const std::vector<Candidate> &analog,
              const std::vector<Candidate> &hdzero, int n_analog, int n_hdzero, const std::vector<Wifi> *wifi,
              const std::vector<Candidate> &pinned) {
    Advice out;
    out.wifi = wifi ? *wifi : find_wifi(bins, Region::Eu, seen);
    for (const auto &c : analog) out.analog.push_back(rate(c, bins, seen, out.wifi));
    for (const auto &c : hdzero) out.hdzero.push_back(rate(c, bins, seen, out.wifi));
    n_analog = std::clamp(n_analog, 0, static_cast<int>(analog.size()));
    n_hdzero = std::clamp(n_hdzero, 0, static_cast<int>(hdzero.size()));
    std::vector<Rating> all = out.analog;
    all.insert(all.end(), out.hdzero.begin(), out.hdzero.end());
    // los fijados: el candidato del mismo nombre y sistema si lo hay (no sale dos veces); si no (fuera de las bandas
    // elegidas), se puntua aparte y se anade al final de all, donde la busqueda no lo ve como libre porque ya esta
    // en todas las elecciones
    std::vector<int> fixed;
    int fixed_a = 0, fixed_h = 0;
    for (const auto &c : pinned) {
        int idx = -1;
        for (int i = 0; i < static_cast<int>(all.size()); i++)
            if (all[i].ch.name == c.name && all[i].ch.hdzero == c.hdzero) idx = i;
        if (idx < 0) {
            all.push_back(rate(c, bins, seen, out.wifi));
            idx = static_cast<int>(all.size()) - 1;
        }
        if (std::find(fixed.begin(), fixed.end(), idx) != fixed.end()) continue;  // fijado repetido
        fixed.push_back(idx);
        (c.hdzero ? fixed_h : fixed_a)++;
    }
    // el total de cada sistema incluye sus fijados; con mas fijados que pilotos, solo los fijados
    n_analog = std::max(n_analog, fixed_a);
    n_hdzero = std::max(n_hdzero, fixed_h);
    if (n_analog + n_hdzero == 0) {
        out.complete = true;
        return out;
    }
    // canales de bandas distintas en la misma frecuencia son candidatos distintos: la separacion minima impide
    // elegir dos a la vez
    Search s{all, n_analog, n_hdzero, fixed};
    std::vector<int> best;
    if (static_cast<int>(fixed.size()) == n_analog + n_hdzero) {
        best = fixed;  // no falta ninguno
    } else {
        float bt = std::numeric_limits<float>::max();
        for (int st = 0; st < static_cast<int>(all.size()); st++) {
            if (!s.fits(fixed, st)) continue;
            auto p = s.greedy(st);
            if (static_cast<int>(p.size()) < n_analog + n_hdzero) continue;
            const float t = s.total(p);
            if (t < bt) {
                bt = t;
                best = p;
            }
        }
        if (best.empty()) best = s.greedy(-1);  // no deberia pasar (los totales caben), pero los fijados salen
        s.improve(best);
    }

    std::vector<const Candidate *> set;
    for (int i : best) set.push_back(&all[i].ch);
    out.complete = true;
    for (size_t k = 0; k < best.size(); k++) {
        const int i = best[k];
        Pick pk;
        pk.r = all[i];
        pk.pinned = k < fixed.size();
        bool close = false;
        for (int j : best)
            if (j != i) {
                if (pair_cost(all[i].ch, all[j].ch, &close) >= kInfeasible) out.complete = false;
            }
        if (close) pk.notes.push_back(Note::TooClose);
        if (imd_hits(set, *set[k]) > 0) pk.notes.push_back(Note::Imd);
        if (all[i].busy >= 0) pk.notes.push_back(Note::Busy);
        if (all[i].coverage < 0.9f) pk.notes.push_back(Note::Partial);
        out.worst_db = std::max(out.worst_db, all[i].noise_db);
        out.picks.push_back(pk);
    }
    std::sort(out.picks.begin(), out.picks.end(), [](const Pick &a, const Pick &b) { return a.r.ch.mhz < b.r.ch.mhz; });
    return out;
}

}  // namespace advisor
