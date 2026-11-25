#include "elrs_advisor.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace elrs {

namespace {
constexpr double kChannelHalfMhz = 0.45;  // ancho de un canal de ELRS (LoRa 812 kHz, FLRC ~1 MHz): +-0.45 MHz
constexpr double kWifiHalfMhz = 8.3;      // un WiFi de 20 MHz ocupa 16.6 MHz
constexpr float kWifiPeakMin = 6, kWifiOverNoise = 7;  // como en src/channel_advisor.h
constexpr double kWifiLitShare = 0.6, kWifiHalfLit = 0.5;
constexpr float kWifiTrafficDb = 3;
// Nivel de un canal de ELRS: ocupado si lo pisan a ratos (o suelo levantado); malo si mucho
// No por la media: la (lineal) levantan mucho rafagas raras pero fuertes (un ELRS a 250 Hz en otra habitacion, con el
// 1-3 % del tiempo en cada canal, subia la media 5-10 dB; medido). A ELRS le importa la parte del tiempo ocupada (duty)
constexpr float kBusyDuty = 0.03f, kBadDuty = 0.15f, kBusyPeakDb = 15;
// El ruido solo tambien pasa a ratos de 6 dB: duty ~2 % (medido con el adaptador, test/channel_advisor_test.cpp). Se
// descuenta, para que una banda vacia no salga ocupada el 2 % del tiempo
constexpr float kNoiseDuty = 0.02f;
// Emisoras que saltan (Bluetooth, otros ELRS): picos cortos en muchos canales fuera de los WiFi
constexpr float kHopperDuty = 0.05f, kHopperShare = 0.25f;
constexpr int kHopperMinChannels = 8;
constexpr float kContDuty = 0.8f, kContMeanDb = 6;
constexpr double kContMinMhz = 1.0;  // mas estrecho: puede ser una espuria del propio adaptador
// Veredicto, por la parte de los saltos que caen ocupados
constexpr float kCleanHit = 0.03f, kHeavyHit = 0.15f, kHeavyBadShare = 0.3f, kHopelessHit = 0.3f;
constexpr int kMinMeasured = kChannels / 2;

float median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

float percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * v.size()));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

double wifi_mhz(int ch) { return 2412 + 5.0 * (ch - 1); }
}  // namespace

namespace {
constexpr Mode kModes[] = {
    {"F1000", true, 1000, 389, 1, 1, -104},         {"F500", true, 2000, 389, 1, 1, -104},
    {"D500", true, 1000, 389, 2, 2, -104},          {"D250", true, 1000, 389, 4, 3, -104},
    {"500Hz", false, 2000, 1507, 1, 1, -105},       {"333Hz Full", false, 3003, 2374, 1, 1, -105},
    {"250Hz", false, 4000, 3300, 1, 1, -108},       {"150Hz", false, 6666, 5871, 1, 1, -112},
    {"100Hz Full", false, 10000, 7605, 1, 1, -112}, {"50Hz", false, 20000, 10798, 1, 1, -115},
};
static_assert(sizeof(kModes) / sizeof(kModes[0]) == static_cast<size_t>(ModeId::Count), "kModes vs ModeId");

}  // namespace

const Mode &mode(ModeId m) { return kModes[static_cast<size_t>(m)]; }

float pilot_collision(const Mode &m) {
    const double window = std::min(1.0, 2.0 * m.toa_us / m.interval_us);
    return static_cast<float>(window / kChannels);
}

float packet_loss(const Mode &m, float hit, int others) {
    const double pilots = 1 - std::pow(1 - pilot_collision(m), std::max(0, others));
    const double q = 1 - (1 - hit) * (1 - pilots);  // un envio, pisado
    return static_cast<float>(std::pow(q, m.freqs));  // DVDA: pisados los envios de todas sus frecuencias
}

Report analyse(const std::vector<Bin> &bins, Use use, Region region, int others) {
    Report r;
    r.others = std::max(0, others);
    r.pilot_hit = 1 - static_cast<float>(std::pow(1 - pilot_collision(mode(ModeId::Hz500)), r.others));
    switch (use) {
    case Use::Racing: r.modes = {ModeId::F1000, ModeId::F500, ModeId::Hz500, ModeId::D500, ModeId::D250}; break;
    case Use::Freestyle: r.modes = {ModeId::Hz500, ModeId::F500, ModeId::Hz250, ModeId::D500, ModeId::D250}; break;
    case Use::LongRange: r.modes = {ModeId::Hz250, ModeId::Hz150, ModeId::Hz100Full, ModeId::Hz50}; break;
    }
    std::vector<float> allPeak;
    for (const auto &b : bins) {
        allPeak.push_back(b.peak_db);
        r.worst_peak = std::max(r.worst_peak, b.peak_db);
    }
    const float lit = std::max(kWifiPeakMin, percentile(allPeak, 0.25) + kWifiOverNoise);

    // ---- redes WiFi: un canal con su ancho encendido (las dos mitades: el de al lado solo enciende una) ----
    struct Cand {
        Wifi w;
        float score;
    };
    std::vector<Cand> cands;
    for (int ch = 1; ch <= 13; ch++) {
        const double c = wifi_mhz(ch);
        int n = 0, nLit = 0, nLo = 0, litLo = 0, nHi = 0, litHi = 0;
        std::vector<float> mean, peak;
        float duty = 0;
        for (const auto &b : bins) {
            const double d = b.mhz - c;
            if (std::fabs(d) > kWifiHalfMhz) continue;
            n++;
            const bool on = b.peak_db >= lit;
            nLit += on;
            (d < 0 ? nLo : nHi)++;
            (d < 0 ? litLo : litHi) += on;
            mean.push_back(b.mean_db);
            peak.push_back(b.peak_db);
            duty += b.duty;
        }
        // medido al menos en tres cuartas partes de su ancho (16.6 MHz / 0.156 = 106 bins)
        if (n < 80 || nLo == 0 || nHi == 0) continue;
        if (nLit < kWifiLitShare * n || litLo < kWifiHalfLit * nLo || litHi < kWifiHalfLit * nHi) continue;
        Wifi w;
        w.ch = ch;
        w.mhz = c;
        w.mean_db = median(mean);
        w.peak_db = median(peak);
        w.duty = duty / n;
        w.traffic = w.mean_db >= kWifiTrafficDb;
        // la media de los picos en su ancho (no la mediana): el canal de al lado, con 5 MHz fuera de la red, da menos
        float peakSum = 0;
        for (float x : peak) peakSum += x;
        cands.push_back({w, peakSum / static_cast<float>(peak.size()) + w.mean_db});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
    for (const auto &c : cands) {
        // el canal de al lado de uno mas fuerte es el mismo WiFi visto 5 MHz movido
        if (std::any_of(r.wifi.begin(), r.wifi.end(), [&](const Wifi &w) { return std::abs(w.ch - c.w.ch) <= 1; }))
            continue;
        r.wifi.push_back(c.w);
    }
    const auto underWifi = [&](double mhz) {
        int best = -1;
        for (size_t i = 0; i < r.wifi.size(); i++)
            if (std::fabs(mhz - r.wifi[i].mhz) <= kWifiHalfMhz && best < 0) best = static_cast<int>(i);
        return best;  // r.wifi va de mas a menos fuerte: el primero que lo pisa
    };

    // ---- los 80 canales de ELRS ----
    float hitSum = 0;
    for (int i = 0; i < kChannels; i++) {
        Channel ch;
        ch.index = i;
        ch.mhz = channel_mhz(i);
        std::vector<float> mean, peak, duty;
        for (const auto &b : bins)
            if (std::fabs(b.mhz - ch.mhz) <= kChannelHalfMhz) {
                mean.push_back(b.mean_db);
                peak.push_back(b.peak_db);
                duty.push_back(b.duty);
            }
        // ~6 bins de 0.156 MHz; con la mitad basta. La mediana: una espuria de un bin no ocupa el canal
        ch.measured = mean.size() >= 3;
        if (ch.measured) {
            ch.mean_db = median(mean);
            ch.peak_db = median(peak);
            ch.duty = std::clamp((median(duty) - kNoiseDuty) / (1 - kNoiseDuty), 0.f, 1.f);
            ch.wifi = underWifi(ch.mhz);
            // por la parte del tiempo ocupada (un suelo levantado mas de 6 dB sale con duty ~1) y por los picos
            if (ch.duty >= kBadDuty) ch.level = Level::Bad;
            else if (ch.duty >= kBusyDuty || ch.peak_db >= kBusyPeakDb) ch.level = Level::Busy;
            else ch.level = Level::Clean;
            r.measured++;
            hitSum += ch.duty;
            (ch.level == Level::Bad ? r.bad : ch.level == Level::Busy ? r.busy : r.clean)++;
        }
        r.channels.push_back(ch);
    }
    r.hit = r.measured > 0 ? hitSum / r.measured : 0;

    // ---- otras emisiones ----
    {
        int n = 0, hop = 0;
        std::vector<float> pk;
        for (const auto &c : r.channels) {
            if (!c.measured || c.wifi >= 0) continue;
            n++;
            // picos sobre el ruido (umbral fijo: con un ELRS por toda la banda el percentil de los picos sube) y
            // poco tiempo encendido; la media no cuenta (la suben las rafagas fuertes)
            if (c.peak_db >= std::max(kNoisePeakDb, lit - 3) && c.duty < kHopperDuty) {
                hop++;
                pk.push_back(c.peak_db);
            }
        }
        if (hop >= kHopperMinChannels && hop >= kHopperShare * n) {
            Emission e;
            e.kind = Other::Hoppers;
            e.lo_mhz = channel_mhz(0);
            e.hi_mhz = channel_mhz(kChannels - 1);
            e.peak_db = median(pk);
            e.share = static_cast<float>(hop) / static_cast<float>(n);
            r.other.push_back(e);
        }
    }
    {
        // tramos encendidos sin parar fuera de los WiFi
        double lo = 0, hi = 0, prev = -1e9;
        float pk = 0;
        const auto flush = [&] {
            if (hi - lo >= kContMinMhz) r.other.push_back({Other::Continuous, lo, hi, pk, 0});
        };
        for (const auto &b : bins) {
            const bool on = b.duty >= kContDuty && b.mean_db >= kContMeanDb && underWifi(b.mhz) < 0;
            if (!on) continue;
            if (b.mhz - prev > 0.5) {
                if (prev > -1e8) flush();
                lo = b.mhz;
                pk = 0;
            }
            hi = b.mhz;
            prev = b.mhz;
            pk = std::max(pk, b.peak_db);
        }
        if (prev > -1e8) flush();
    }
    if (r.worst_peak >= kStrongDb) {
        double at = 0;
        for (const auto &b : bins)
            if (b.peak_db == r.worst_peak) at = b.mhz;
        r.other.push_back({Other::Strong, at - 1, at + 1, r.worst_peak, 0});
    }

    // ---- veredicto y sugerencias ----
    if (r.measured < kMinMeasured) {
        r.verdict = Verdict::Unknown;
        return r;
    }
    const float badShare = static_cast<float>(r.bad) / static_cast<float>(r.measured);
    // lo medido y los pilotos que van a volar a la vez (a 500 Hz)
    const float hit = 1 - (1 - r.hit) * (1 - r.pilot_hit);
    if (hit >= kHeavyHit || badShare >= kHeavyBadShare) r.verdict = Verdict::Heavy;
    else if (hit < kCleanHit && r.bad <= 2) r.verdict = Verdict::Clean;
    else r.verdict = Verdict::Moderate;

    r.tips = tips_for(r, region);
    return r;
}

namespace {
constexpr Tip kTipOrder[] = {Tip::RateClean, Tip::RateModerate, Tip::RateHeavy, Tip::Gemini,   Tip::Band900,
                             Tip::Pilots,    Tip::Strong,       Tip::OwnWifi,   Tip::Continuous, Tip::Hoppers,
                             Tip::LbtBusy,   Tip::DynamicPower, Tip::Telemetry, Tip::WifiOnTx,  Tip::Antennas};
}  // namespace

int tip_order(Tip t) {
    for (size_t i = 0; i < std::size(kTipOrder); i++)
        if (kTipOrder[i] == t) return static_cast<int>(i);
    return static_cast<int>(std::size(kTipOrder));
}

std::vector<Tip> tips_for(const Report &r, Region region) {
    std::vector<Tip> t;
    if (r.verdict == Verdict::Unknown) return t;
    const auto has = [&](Other k) {
        return std::any_of(r.other.begin(), r.other.end(), [&](const Emission &e) { return e.kind == k; });
    };
    const auto add = [&](Tip x) {
        if (std::find(t.begin(), t.end(), x) == t.end()) t.push_back(x);
    };
    switch (r.verdict) {
    case Verdict::Clean: add(Tip::RateClean); break;
    case Verdict::Moderate: add(Tip::RateModerate); add(Tip::Gemini); break;
    case Verdict::Heavy:
        add(Tip::RateHeavy);
        add(Tip::Gemini);
        if (r.hit >= kHopelessHit) add(Tip::Band900);
        break;
    case Verdict::Unknown: break;
    }
    if (r.others > 0) {
        add(Tip::Pilots);
        if (r.others >= kManyPilots) add(Tip::Gemini);
    }
    if (has(Other::Strong)) add(Tip::Strong);
    if (std::any_of(r.wifi.begin(), r.wifi.end(), [](const Wifi &w) { return w.peak_db >= kWifiStrongDb || w.traffic; }))
        add(Tip::OwnWifi);
    if (has(Other::Continuous)) add(Tip::Continuous);
    if (has(Other::Hoppers)) add(Tip::Hoppers);
    if (r.verdict != Verdict::Clean) {
        if (region == Region::EuLbt) add(Tip::LbtBusy);
        add(Tip::DynamicPower);
    }
    add(Tip::Telemetry);
    add(Tip::WifiOnTx);  // la WiFi del propio ELRS (TX, receptor, backpack): vale siempre, haya otras redes o no
    add(Tip::Antennas);
    std::stable_sort(t.begin(), t.end(), [](Tip a, Tip b) { return tip_order(a) < tip_order(b); });
    return t;
}

}  // namespace elrs
