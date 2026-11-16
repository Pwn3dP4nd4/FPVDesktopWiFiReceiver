#include "channel_advisor.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "spectrum.h"  // kp::fpv_channels()

namespace advisor {

namespace {
constexpr float kBusyCost = 60;        // ya hay un piloto: solo si no queda otro remedio
constexpr float kUnknownCost = 20;     // menos de la mitad del canal analizado
constexpr float kBurstWeight = 10;     // dB por fraccion del ancho con rafagas (WiFi)
constexpr double kSpacingCost = 0.4;   // dB por MHz por debajo de la separacion ideal
constexpr double kSpreadCost = 40;     // dB*MHz: 1 dB a 40 MHz, 0.5 dB a 80 MHz (separar mas, si se puede)
constexpr float kImdCost = 3;          // dB por producto de intermodulacion encima de otro canal
constexpr double kImdReach = 4;        // MHz: un producto a menos de esto de un canal lo afecta
constexpr float kInfeasible = 1000;    // separacion por debajo de la minima

double width(bool hdz) { return hdz ? kHdzBw : kAnalogBw; }

double sig_width(const Signal &s) { return s.digital && s.bw_mhz > 0 ? s.bw_mhz : kAnalogBw; }

float pair_cost(const Candidate &a, const Candidate &b, bool *close) {
    const double d = std::fabs(a.mhz - b.mhz);
    if (d < min_spacing(a.hdzero, b.hdzero)) return kInfeasible;
    const double ideal = ideal_spacing(a.hdzero, b.hdzero);
    // a little for every pair, less the further apart: with a clean band the pilots spread out
    const float spread = static_cast<float>(kSpreadCost / d);
    if (d >= ideal) return spread;
    if (close) *close = true;
    return spread + static_cast<float>((ideal - d) * kSpacingCost);
}

// 2*f1 - f2 de cada par analogico sobre un tercer canal analogico elegido
int imd_hits(const std::vector<const Candidate *> &set, const Candidate &c) {
    int hits = 0;
    for (const auto *a : set)
        for (const auto *b : set) {
            if (a == b || a->hdzero || b->hdzero) continue;
            if (std::fabs(2 * a->mhz - b->mhz - c.mhz) < kImdReach) hits++;
        }
    return hits;
}

struct Search {
    const std::vector<Rating> &all;  // analogicos y HDZero juntos
    int need_a, need_h;

    float total(const std::vector<int> &pick) const {
        float t = 0;
        for (size_t i = 0; i < pick.size(); i++) {
            t += all[pick[i]].cost;
            for (size_t j = i + 1; j < pick.size(); j++) t += pair_cost(all[pick[i]].ch, all[pick[j]].ch, nullptr);
        }
        std::vector<const Candidate *> set;
        for (int i : pick) set.push_back(&all[i].ch);
        for (int i : pick)
            if (!all[i].ch.hdzero) t += kImdCost * static_cast<float>(imd_hits(set, all[i].ch));
        return t;
    }

    bool fits(const std::vector<int> &pick, int i) const {
        if (std::find(pick.begin(), pick.end(), i) != pick.end()) return false;
        int a = 0, h = 0;
        for (int p : pick) (all[p].ch.hdzero ? h : a)++;
        return all[i].ch.hdzero ? h < need_h : a < need_a;
    }

    std::vector<int> greedy(int start) const {
        std::vector<int> pick{start};
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
            for (size_t k = 0; k < pick.size(); k++)
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

std::vector<Candidate> hdzero_channels(const std::string &bands) {
    static const char *kNames[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "E1", "F1", "F2", "F4",
                                   "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8"};
    std::vector<Candidate> out;
    for (const char *n : kNames) {
        if (bands.find(n[0]) == std::string::npos) continue;
        for (const auto &c : kp::fpv_channels())
            if (std::string(c.name) == n) out.push_back(Candidate{c.name, c.mhz, true});
    }
    return out;
}

double min_spacing(bool a_hdz, bool b_hdz) {
    if (a_hdz && b_hdz) return 27;
    if (a_hdz || b_hdz) return 24;  // 10 + 13.5 MHz: los bordes se tocan
    return 20;
}

double ideal_spacing(bool a_hdz, bool b_hdz) {
    if (a_hdz && b_hdz) return 37;  // la rejilla de canales de HDZero
    if (a_hdz || b_hdz) return 40;
    return 37;                      // Raceband
}

Rating rate(const Candidate &c, const std::vector<Bin> &bins, const std::vector<Signal> &seen) {
    Rating r;
    r.ch = c;
    const double half = width(c.hdzero) / 2;
    double lin = 0;
    int n = 0, bursty = 0;
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (const auto &b : bins) {
        if (std::fabs(b.mhz - c.mhz) > half) continue;
        lin += std::pow(10.0, b.mean_db / 10.0);
        bursty += b.peak_db > 10 && b.peak_db - b.mean_db > 10;
        lo = std::min(lo, b.mhz);
        hi = std::max(hi, b.mhz);
        n++;
    }
    r.coverage = n ? static_cast<float>(std::min(1.0, (hi - lo + 0.15625) / (2 * half))) : 0.f;
    r.noise_db = n ? static_cast<float>(10 * std::log10(lin / n)) : 0.f;
    r.burst = n ? static_cast<float>(bursty) / static_cast<float>(n) : 0.f;
    r.cost = r.noise_db + kBurstWeight * r.burst;
    if (r.coverage < 0.5f) r.cost += kUnknownCost;
    for (size_t i = 0; i < seen.size(); i++) {
        const double gap = std::fabs(seen[i].mhz - c.mhz) - (sig_width(seen[i]) + width(c.hdzero)) / 2;
        if (gap < 0) {
            r.busy = static_cast<int>(i);
            r.cost += kBusyCost;
            break;
        }
    }
    return r;
}

Advice advise(const std::vector<Bin> &bins, const std::vector<Signal> &seen, const std::vector<Candidate> &analog,
              const std::vector<Candidate> &hdzero, int n_analog, int n_hdzero) {
    Advice out;
    for (const auto &c : analog) out.analog.push_back(rate(c, bins, seen));
    for (const auto &c : hdzero) out.hdzero.push_back(rate(c, bins, seen));
    n_analog = std::clamp(n_analog, 0, static_cast<int>(analog.size()));
    n_hdzero = std::clamp(n_hdzero, 0, static_cast<int>(hdzero.size()));
    if (n_analog + n_hdzero == 0) {
        out.complete = true;
        return out;
    }
    std::vector<Rating> all = out.analog;
    all.insert(all.end(), out.hdzero.begin(), out.hdzero.end());
    // canales de bandas distintas en la misma frecuencia son candidatos distintos: la separacion minima impide
    // elegir dos a la vez
    Search s{all, n_analog, n_hdzero};
    std::vector<int> best;
    float bt = std::numeric_limits<float>::max();
    for (int st = 0; st < static_cast<int>(all.size()); st++) {
        if (!s.fits({}, st)) continue;
        auto p = s.greedy(st);
        if (static_cast<int>(p.size()) < n_analog + n_hdzero) continue;
        const float t = s.total(p);
        if (t < bt) {
            bt = t;
            best = p;
        }
    }
    s.improve(best);

    std::vector<const Candidate *> set;
    for (int i : best) set.push_back(&all[i].ch);
    out.complete = true;
    for (int i : best) {
        Pick pk;
        pk.r = all[i];
        bool close = false;
        for (int j : best)
            if (j != i) {
                if (pair_cost(all[i].ch, all[j].ch, &close) >= kInfeasible) out.complete = false;
            }
        if (close) pk.notes.push_back(Note::TooClose);
        if (!all[i].ch.hdzero && imd_hits(set, all[i].ch) > 0) pk.notes.push_back(Note::Imd);
        if (all[i].busy >= 0) pk.notes.push_back(Note::Busy);
        if (all[i].coverage < 0.9f) pk.notes.push_back(Note::Partial);
        out.worst_db = std::max(out.worst_db, all[i].noise_db);
        out.picks.push_back(pk);
    }
    std::sort(out.picks.begin(), out.picks.end(), [](const Pick &a, const Pick &b) { return a.r.ch.mhz < b.r.ch.mhz; });
    return out;
}

}  // namespace advisor
