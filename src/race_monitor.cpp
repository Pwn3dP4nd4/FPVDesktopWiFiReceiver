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

// Una senal es la del piloto de un fijado si es del mismo sistema y su centro esta cerca: un VTX analogico se
// desvia unos MHz de su canal y un HDZero se mide con el error del barrido. Un analogico encima de un HDZero (o al
// reves) no es su piloto aunque este en la misma frecuencia: es otro sistema, un intruso.
bool matches(const advisor::Signal &s, const advisor::Candidate &c) {
    return s.digital == c.hdzero && std::fabs(s.mhz - c.mhz) < kMatchMhz;
}

// Una senal ajena pisa el canal si sus anchos se solapan (el gap < 0 de advisor::rate); una digital junto a un canal
// analogico, tambien si queda a menos de kDigitalGuardMhz de su borde
bool overlaps(const advisor::Signal &s, const advisor::Candidate &c) {
    const double gap = std::fabs(s.mhz - c.mhz) - (advisor::width(s) + advisor::width(c)) / 2;
    return gap < (s.digital && !c.hdzero ? kDigitalGuardMhz : 0);
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
    leaving_.clear();
    leaving_ch_.clear();
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

std::vector<int> Monitor::match_pilots(const std::vector<advisor::Signal> &now) const {
    // De cada senal, el fijado con el que casa (el mas cercano, por si dos fijados estuvieran a menos de kMatchMhz)
    // y de cada fijado, la senal de su piloto (la mas cercana, si hubiera dos)
    const size_t n = health_.size();
    std::vector<int> pilot(n, -1);
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
    return pilot;
}

std::vector<bool> Monitor::own_signals(const std::vector<advisor::Signal> &now, const std::vector<int> &pilot) {
    // solo la de su piloto es propia: otra que tambien case con el mismo fijado es de alguien mas (un intruso)
    std::vector<bool> own(now.size(), false);
    for (int p : pilot)
        if (p >= 0) own[p] = true;
    // un piloto movido que aun esta en su canal viejo: tambien es propio (ni intruso ni ajeno)
    for (auto &l : leaving_)
        for (size_t s = 0; s < now.size(); s++)
            if (!own[s] && matches(now[s], l.ch)) {
                own[s] = true;
                l.seen = sweeps_;
            }
    leaving_.erase(std::remove_if(leaving_.begin(), leaving_.end(),
                                  [&](const Leaving &l) {
                                      return sweeps_ - l.seen >= kLeaveSweeps || sweeps_ - l.since >= kLeaveMaxSweeps;
                                  }),
                   leaving_.end());
    leaving_ch_.clear();
    for (const auto &l : leaving_) leaving_ch_.push_back(l.ch);
    return own;
}

void Monitor::remember_foreign() {
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
}

void Monitor::update_health(size_t i, int pilot, const std::vector<advisor::Signal> &now,
                            const std::vector<advisor::Wifi> &wifi) {
    ChannelHealth &h = health_[i];
    const int sw = h.sweeps++;  // indice local: desde start() o desde que se movio
    if (pilot >= 0) {
        h.seen++;
        // la SNR, solo de una medida de este barrido: la de un piloto que sigue en el aire sin medirse de nuevo
        // es la de antes (o 0, segun el backend) y contaria otra vez
        if (now[pilot].fresh) snr_[i].push_back(now[pilot].snr_db);
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

void Monitor::update(const std::vector<advisor::Bin> &bins, const std::vector<advisor::Signal> &now,
                     const std::vector<advisor::Wifi> &wifi) {
    sweeps_++;
    const std::vector<int> pilot = match_pilots(now);
    const std::vector<bool> own = own_signals(now, pilot);
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
    remember_foreign();
    for (size_t i = 0; i < health_.size(); i++) update_health(i, pilot[i], now, wifi);
}

void Monitor::moved(int i, const advisor::Candidate &c) {
    if (i < 0 || i >= static_cast<int>(health_.size())) return;
    const advisor::Candidate old = health_[i].ch;
    reset(static_cast<size_t>(i));
    health_[i].ch = c;
    if (old.hdzero == c.hdzero && std::fabs(old.mhz - c.mhz) < kMatchMhz) return;  // el mismo canal (otro ancho)
    leaving_.push_back({old, sweeps_, sweeps_});
    leaving_ch_.push_back(old);
}

int present(const std::vector<advisor::Candidate> &pinned, const std::vector<advisor::Signal> &now) {
    std::vector<bool> got(pinned.size(), false);
    for (const auto &s : now) {
        int best = -1;
        for (size_t i = 0; i < pinned.size(); i++)
            if (matches(s, pinned[i]) &&
                (best < 0 || std::fabs(s.mhz - pinned[i].mhz) < std::fabs(s.mhz - pinned[best].mhz)))
                best = static_cast<int>(i);
        if (best >= 0) got[best] = true;
    }
    return static_cast<int>(std::count(got.begin(), got.end(), true));
}

int Monitor::worst() const {
    int w = -1;
    for (size_t i = 0; i < health_.size(); i++) {
        if (health_[i].seen == 0) continue;  // no ha salido: no hay video que juzgar
        if (w < 0 || health_[i].quality < health_[w].quality) w = static_cast<int>(i);
    }
    return w;
}

std::vector<int> Monitor::move_order(const std::vector<Move> &prev) const {
    // De peor a mejor video: mover a un piloto en plena carrera tiene un coste (aterrizar, cambiar el VTX), asi que
    // no se busca el reparto optimo sino arreglar a los que lo estan pasando peor, uno a uno
    std::vector<int> order;
    for (size_t i = 0; i < health_.size(); i++)
        if (health_[i].seen > 0) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return health_[a].quality < health_[b].quality; });
    // los de las recomendaciones anteriores, primero (en su orden): que no salten por poco
    std::vector<int> first;
    for (const auto &pv : prev)
        if (std::find(order.begin(), order.end(), pv.from) != order.end() &&
            std::find(first.begin(), first.end(), pv.from) == first.end())
            first.push_back(pv.from);
    for (int i : order)
        if (std::find(first.begin(), first.end(), i) == first.end()) first.push_back(i);
    return first;
}

std::vector<advisor::Candidate> Monitor::free_channels(const ChannelHealth &h,
                                                       const std::vector<advisor::Candidate> &pool,
                                                       const std::vector<advisor::Signal> &foreign) const {
    // candidatos de su sistema, sin el canal en que ya esta (ni otro casi en la misma frecuencia: otra banda) ni
    // los que esten a menos de la separacion minima de una senal ajena: lo que esta tiene pegado a su canal no se
    // ve si cae bajo la portadora de un fijado (se quita con su senal), asi que se le pide lo mismo que a un fijado
    std::vector<advisor::Candidate> cand;
    for (const auto &c : pool) {
        bool ok = c.hdzero == h.ch.hdzero && std::fabs(c.mhz - h.ch.mhz) >= kMatchMhz;
        for (const auto &f : foreign) {
            const advisor::Candidate fc{f.name, f.mhz, f.digital, f.digital ? f.bw_mhz : 0};
            ok = ok && std::fabs(f.mhz - c.mhz) >= advisor::min_spacing(fc, c);
        }
        // ni los que acaba de dejar un piloto movido: hasta que aterrice y cambie sigue emitiendo ahi (su senal
        // cuenta como suya y esta quitada del espectro: el canal se veria libre y limpio)
        for (const auto &l : leaving_ch_) ok = ok && std::fabs(l.mhz - c.mhz) >= advisor::min_spacing(l, c);
        if (ok) cand.push_back(c);
    }
    return cand;
}

std::optional<Move> Monitor::suggest_for(int i, const std::vector<advisor::Candidate> &chans,
                                         const std::vector<advisor::Candidate> &cand,
                                         const std::vector<advisor::Signal> &foreign,
                                         const std::vector<Move> &prev) const {
    const ChannelHealth &h = health_[i];
    std::vector<advisor::Candidate> others;
    int n_a = 0, n_h = 0;
    for (size_t j = 0; j < chans.size(); j++) {
        if (static_cast<int>(j) == i) continue;
        others.push_back(chans[j]);
        (chans[j].hdzero ? n_h : n_a)++;
    }
    (h.ch.hdzero ? n_h : n_a)++;  // el que se busca
    const std::vector<advisor::Candidate> none;
    const auto adv = advisor::advise(last_bins_, foreign, h.ch.hdzero ? none : cand, h.ch.hdzero ? cand : none,
                                     n_a, n_h, &wifi_, others);
    // el elegido por la busqueda (no fijado) de su sistema; se comprueba que no pisa a los demas por si no
    // hubiera sitio (advise lo devuelve igual, con complete = false)
    const auto fits = [&](const advisor::Candidate &c) {
        bool ok = std::fabs(c.mhz - h.ch.mhz) >= kMatchMhz;
        for (const auto &o : others) ok = ok && std::fabs(o.mhz - c.mhz) >= advisor::min_spacing(o, c);
        return ok;
    };
    const advisor::Pick *best = nullptr;
    for (const auto &pk : adv.picks) {
        if (pk.pinned || pk.r.ch.hdzero != h.ch.hdzero || !fits(pk.r.ch)) continue;
        if (!best || pk.r.cost < best->r.cost) best = &pk;
    }
    // lo que se gana: lo atribuible a su canal actual (interferencia e intrusos) contra el coste del nuevo. La
    // SNR baja y los cortes no entran: con el canal limpio no se arreglan cambiando de canal
    advisor::Candidate to;
    float gain = -1;
    if (best) {
        to = best->r.ch;
        gain = h.penalty - best->r.cost;
    }
    // la anterior se mantiene si sigue valiendo (con menos mejora que para salir: kMoveKeepGainDb) y la nueva no
    // es claramente mejor
    bool kept = false;
    const auto pv = std::find_if(prev.begin(), prev.end(), [&](const Move &m) { return m.from == i; });
    if (pv != prev.end()) {
        const advisor::Candidate *pc = nullptr;
        for (const auto &c : cand)
            if (c.name == pv->to.name && c.hdzero == pv->to.hdzero) pc = &c;
        if (pc && fits(*pc)) {
            const float pg = h.penalty - advisor::rate(*pc, last_bins_, foreign, wifi_).cost;
            if (pg >= kMoveKeepGainDb && gain < pg + kMoveMinGainDb) {
                to = *pc;
                gain = pg;
                kept = true;
            }
        }
    }
    if (gain < (kept ? kMoveKeepGainDb : kMoveMinGainDb)) return std::nullopt;
    Move mv;
    mv.from = i;
    mv.to = to;
    mv.gain_db = gain;
    mv.cause = static_cast<float>(h.intruders) / static_cast<float>(h.sweeps) > 0.2f ? Cause::Intruder
                                                                                       : Cause::Interference;
    return mv;
}

std::vector<Move> Monitor::suggest_all(const std::vector<advisor::Candidate> &analog,
                                       const std::vector<advisor::Candidate> &hdzero,
                                       const std::vector<Move> &prev) const {
    // cada recomendacion cuenta con las anteriores ya aplicadas (sus canales nuevos), para que se puedan aplicar
    // todas a la vez
    std::vector<advisor::Candidate> chans;  // el reparto con las recomendaciones de este bucle aplicadas
    for (const auto &h : health_) chans.push_back(h.ch);
    std::vector<advisor::Signal> foreign;   // las ajenas recordadas: ocupadas y a guardar separacion con ellas
    for (const auto &r : recent_) foreign.push_back(r.s);
    std::vector<Move> out;
    for (int i : move_order(prev)) {
        const ChannelHealth &h = health_[i];
        if (h.sweeps < kMinSweeps) continue;
        const auto cand = free_channels(h, h.ch.hdzero ? hdzero : analog, foreign);
        if (cand.empty()) continue;
        const auto mv = suggest_for(i, chans, cand, foreign, prev);
        if (!mv) continue;
        out.push_back(*mv);
        chans[i] = mv->to;
    }
    return out;
}

std::optional<Move> Monitor::suggest(const std::vector<advisor::Candidate> &analog,
                                     const std::vector<advisor::Candidate> &hdzero, const Move *prev) const {
    const auto all = suggest_all(analog, hdzero, prev ? std::vector<Move>{*prev} : std::vector<Move>{});
    if (all.empty()) return std::nullopt;
    return all.front();
}

}  // namespace race
