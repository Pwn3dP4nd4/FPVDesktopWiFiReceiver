#include "race_monitor.h"

#include <algorithm>
#include <cmath>

namespace race {

namespace {
// La SNR de un piloto en un barrido es un numero suelto (un paso del barrido mide unos ms): la mediana dice como le
// va normalmente y el percentil 10 como de mal se ve en sus peores momentos, que es lo que nota el piloto (cortes de
// imagen) y lo que se usa para la calidad. Con interpolacion entre los dos valores vecinos: con pocos barridos un
// valor suelto no salta de golpe de un extremo al otro.
float percentile(const std::vector<float> &sorted, double p) {
    if (sorted.empty()) return 0;
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double t = pos - static_cast<double>(lo);
    return static_cast<float>(sorted[lo] + (sorted[hi] - sorted[lo]) * t);
}

double sig_width(const advisor::Signal &s) { return s.digital && s.bw_mhz > 0 ? s.bw_mhz : advisor::kAnalogBw; }

// Una senal es la del piloto de un fijado si es del mismo sistema y su centro esta cerca: un VTX analogico se
// desvia unos MHz de su canal y un HDZero se mide con el error del barrido. Un analogico encima de un HDZero (o al
// reves) no es su piloto aunque este en la misma frecuencia: es otro sistema, un intruso.
bool matches(const advisor::Signal &s, const advisor::Candidate &c) {
    return s.digital == c.hdzero && std::fabs(s.mhz - c.mhz) < kMatchMhz;
}

// Una senal ajena pisa el canal si sus anchos se solapan (el gap < 0 de advisor::rate)
bool overlaps(const advisor::Signal &s, const advisor::Candidate &c) {
    return std::fabs(s.mhz - c.mhz) - (sig_width(s) + advisor::width(c)) / 2 < 0;
}
}  // namespace

void Monitor::start(const std::vector<advisor::Candidate> &pinned) {
    health_.clear();
    for (const auto &c : pinned) {
        ChannelHealth h;
        h.ch = c;
        health_.push_back(h);
    }
    const size_t n = health_.size();
    snr_.assign(n, {});
    first_.assign(n, -1);
    last_.assign(n, -1);
    cost_sum_.assign(n, 0);
    last_bins_.clear();
    foreign_.clear();
    recent_.clear();
    wifi_.clear();
    sweeps_ = 0;
}

void Monitor::reset(size_t i) {
    ChannelHealth h;
    h.ch = health_[i].ch;
    health_[i] = h;
    snr_[i].clear();
    first_[i] = last_[i] = -1;
    cost_sum_[i] = 0;
}

void Monitor::update(const std::vector<advisor::Bin> &bins, const std::vector<advisor::Signal> &now,
                     const std::vector<advisor::Wifi> &wifi) {
    const size_t n = health_.size();
    sweeps_++;
    // De cada senal, el fijado con el que casa (el mas cercano, por si dos fijados estuvieran a menos de kMatchMhz)
    // y de cada fijado, la senal de su piloto (la mas cercana, si hubiera dos)
    std::vector<int> pilot(n, -1);
    std::vector<bool> own(now.size(), false);
    for (size_t s = 0; s < now.size(); s++) {
        int best = -1;
        double bd = kMatchMhz;
        for (size_t i = 0; i < n; i++) {
            if (!matches(now[s], health_[i].ch)) continue;
            const double d = std::fabs(now[s].mhz - health_[i].ch.mhz);
            if (d < bd) {
                bd = d;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) continue;
        const int p = pilot[best];
        if (p < 0 || bd < std::fabs(now[p].mhz - health_[best].ch.mhz)) pilot[best] = static_cast<int>(s);
    }
    // solo la de su piloto es propia: otra que tambien case con el mismo fijado es de alguien mas (un intruso)
    for (size_t i = 0; i < n; i++)
        if (pilot[i] >= 0) own[pilot[i]] = true;

    // Las senales de los pilotos propios no son interferencia de nadie (ni del suyo ni de los vecinos, por las
    // bandas laterales): se quitan todas del espectro. Lo que queda debajo de cada portadora no se ve, pero si lo
    // que asoma fuera de su ancho, las redes recordadas y el canal entero en los barridos en que su piloto no esta.
    last_bins_ = bins;
    for (size_t s = 0; s < now.size(); s++)
        if (own[s]) advisor::mask_signal(last_bins_, now[s], now);  // el nivel de alrededor, sin otras senales
    foreign_.clear();
    for (size_t s = 0; s < now.size(); s++)
        if (!own[s]) foreign_.push_back(now[s]);
    wifi_ = wifi;
    // las ajenas de los ultimos kForeignMemory barridos (una que sale a ratos tambien cuenta para suggest)
    for (const auto &f : foreign_) {
        bool found = false;
        for (auto &r : recent_)
            if (r.s.digital == f.digital && std::fabs(r.s.mhz - f.mhz) < kMatchMhz) {
                r.s = f;
                r.sweep = sweeps_;
                found = true;
                break;
            }
        if (!found) recent_.push_back({f, sweeps_});
    }
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const Recent &r) { return sweeps_ - r.sweep >= kForeignMemory; }),
                  recent_.end());

    for (size_t i = 0; i < n; i++) {
        ChannelHealth &h = health_[i];
        const int sw = h.sweeps++;  // indice local: desde start() o desde que se movio
        if (pilot[i] >= 0) {
            h.seen++;
            snr_[i].push_back(now[pilot[i]].snr_db);
            if (first_[i] < 0) first_[i] = sw;
            last_[i] = sw;
        }
        // las senales ajenas que pisan su canal cuentan en intruders; su energia se quita aqui para no contarlas
        // otra vez en la interferencia del canal (lo que asoma de las que solo estan cerca si cuenta)
        const std::vector<advisor::Bin> *cb = &last_bins_;
        std::vector<advisor::Bin> masked;
        for (const auto &f : foreign_)
            if (overlaps(f, h.ch)) {
                if (cb == &last_bins_) {
                    h.intruders++;
                    masked = last_bins_;
                    cb = &masked;
                }
                advisor::mask_signal(masked, f, now);
            }
        // sin vecinos (seen vacio): lo que se mide es el canal, no si esta ocupado (los ajenos van en intruders)
        cost_sum_[i] += advisor::rate(h.ch, *cb, {}, wifi).cost;

        auto v = snr_[i];
        std::sort(v.begin(), v.end());
        h.snr_med = percentile(v, 0.5);
        h.snr_low = percentile(v, 0.1);
        // solo cuenta como corte lo que falta entre la primera y la ultima vez que se vio: antes aun no habia
        // salido y despues ya ha aterrizado (o se ha quedado sin bateria: eso no lo arregla un canal)
        const int span = first_[i] < 0 ? 0 : last_[i] - first_[i] + 1;
        h.lost = span - h.seen;
        h.channel_cost = static_cast<float>(cost_sum_[i] / h.sweeps);
        h.penalty = h.channel_cost + kIntruderCost * static_cast<float>(h.intruders) / static_cast<float>(h.sweeps);
        h.quality = h.snr_low - h.penalty;
        if (span > 0) h.quality -= kLostCost * static_cast<float>(h.lost) / static_cast<float>(span);
    }
}

void Monitor::moved(int i, const advisor::Candidate &c) {
    if (i < 0 || i >= static_cast<int>(health_.size())) return;
    reset(static_cast<size_t>(i));
    health_[i].ch = c;
}

int Monitor::worst() const {
    int w = -1;
    for (size_t i = 0; i < health_.size(); i++) {
        if (health_[i].seen == 0) continue;  // no ha salido: no hay video que juzgar
        if (w < 0 || health_[i].quality < health_[w].quality) w = static_cast<int>(i);
    }
    return w;
}

std::optional<Move> Monitor::suggest(const std::vector<advisor::Candidate> &analog,
                                     const std::vector<advisor::Candidate> &hdzero) const {
    // Solo los de peor video: mover a un piloto en plena carrera tiene un coste (aterrizar, cambiar el VTX), asi que
    // no se busca el reparto optimo sino arreglar a los que lo estan pasando peor
    std::vector<int> order;
    for (size_t i = 0; i < health_.size(); i++)
        if (health_[i].seen > 0) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return health_[a].quality < health_[b].quality; });
    const size_t half = (health_.size() + 1) / 2;
    if (order.size() > half) order.resize(half);

    std::vector<advisor::Signal> foreign;  // las ajenas recordadas: ocupadas y a guardar separacion con ellas
    for (const auto &r : recent_) foreign.push_back(r.s);
    for (int i : order) {
        const ChannelHealth &h = health_[i];
        if (h.sweeps < kMinSweeps) continue;
        std::vector<advisor::Candidate> others;
        int n_a = 0, n_h = 0;
        for (size_t j = 0; j < health_.size(); j++) {
            if (static_cast<int>(j) == i) continue;
            others.push_back(health_[j].ch);
            (health_[j].ch.hdzero ? n_h : n_a)++;
        }
        (h.ch.hdzero ? n_h : n_a)++;  // el que se busca
        // candidatos de su sistema, sin el canal en que ya esta (ni otro casi en la misma frecuencia: otra banda) ni
        // los que esten a menos de la separacion minima de una senal ajena: lo que esta tiene pegado a su canal no se
        // ve si cae bajo la portadora de un fijado (se quita con su senal), asi que se le pide lo mismo que a un fijado
        std::vector<advisor::Candidate> cand;
        for (const auto &c : h.ch.hdzero ? hdzero : analog) {
            bool ok = c.hdzero == h.ch.hdzero && std::fabs(c.mhz - h.ch.mhz) >= kMatchMhz;
            for (const auto &f : foreign) {
                const advisor::Candidate fc{f.name, f.mhz, f.digital, f.digital ? f.bw_mhz : 0};
                ok = ok && std::fabs(f.mhz - c.mhz) >= advisor::min_spacing(fc, c);
            }
            if (ok) cand.push_back(c);
        }
        if (cand.empty()) continue;
        const std::vector<advisor::Candidate> none;
        const auto adv = advisor::advise(last_bins_, foreign, h.ch.hdzero ? none : cand, h.ch.hdzero ? cand : none,
                                         n_a, n_h, &wifi_, others);
        // el elegido por la busqueda (no fijado) de su sistema; se comprueba que no pisa a los demas fijados por si
        // no hubiera sitio (advise lo devuelve igual, con complete = false)
        const advisor::Pick *best = nullptr;
        for (const auto &pk : adv.picks) {
            if (pk.pinned || pk.r.ch.hdzero != h.ch.hdzero) continue;
            bool ok = std::fabs(pk.r.ch.mhz - h.ch.mhz) >= kMatchMhz;
            for (const auto &o : others) ok = ok && std::fabs(o.mhz - pk.r.ch.mhz) >= advisor::min_spacing(o, pk.r.ch);
            if (ok && (!best || pk.r.cost < best->r.cost)) best = &pk;
        }
        if (!best) continue;
        // lo que se gana: lo atribuible a su canal actual (interferencia e intrusos) contra el coste del nuevo. La
        // SNR baja y los cortes no entran: con el canal limpio no se arreglan cambiando de canal
        const float gain = h.penalty - best->r.cost;
        if (gain < kMoveMinGainDb) continue;
        Move mv;
        mv.from = i;
        mv.to = best->r.ch;
        mv.gain_db = gain;
        mv.cause = static_cast<float>(h.intruders) / static_cast<float>(h.sweeps) > 0.2f ? Cause::Intruder
                                                                                           : Cause::Interference;
        return mv;
    }
    return std::nullopt;
}

}  // namespace race
