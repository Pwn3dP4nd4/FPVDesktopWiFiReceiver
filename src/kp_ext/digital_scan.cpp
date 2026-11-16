#include "digital_scan.h"

#include <algorithm>
#include <cmath>

#include "log.h"       // kepyur: KP_LOGI
#include "spectrum.h"  // kepyur: kp::fft_inplace

namespace kpx {

namespace {
constexpr float kAboveFloorDb = 6;     // la banda ocupada: lo que pasa de suelo + 6 dB (como find_humps)
constexpr float kMinSnrDb = 8;         // nivel mediano del bloque sobre el suelo
constexpr double kMinVisible = 6e6;    // un trozo menor no se considera (ni para unirlo con el paso vecino)
constexpr double kJoinGap = 0.5e6;     // huecos estrechos dentro del bloque (portadora nula en DC, etc.)
constexpr double kClipMargin = 0.3e6;  // a menos de esto del borde del margen util: cortado
constexpr float kMinFlat = 0.8f;       // OFDM ~0.95; FM con imagen, menos
constexpr float kMinDuty = 0.9f;       // HDZero emite sin parar; el WiFi va a rafagas
constexpr double kMinWidth = 12e6, kMaxWidth = 31e6;
constexpr double kHdzWideMin = 22e6, kHdzNarrowMin = 14e6;  // 27 y 17 MHz nominales
constexpr double kHdzMaxOffMHz = 2;    // centro medido frente al canal (cristales del VTX y del adaptador)

std::vector<float> median_smooth(const std::vector<float> &db, double bin) {
    // como el suavizado de kepyur (spectrum.cpp): mediana en +-0.3 MHz, quita espurias estrechas
    const int n = static_cast<int>(db.size());
    const int hw = std::max(2, static_cast<int>(0.3e6 / bin));
    std::vector<float> sm(n), win;
    for (int i = 0; i < n; i++) {
        win.clear();
        for (int k = std::max(0, i - hw); k <= std::min(n - 1, i + hw); k++) win.push_back(db[k]);
        std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
        sm[i] = win[win.size() / 2];
    }
    return sm;
}

float median_of(std::vector<float> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
}  // namespace

std::vector<WideRegion> find_wide_regions(const std::vector<float> &db, double fs, double edge_hz) {
    std::vector<WideRegion> out;
    const int n = static_cast<int>(db.size());
    if (n < 64) return out;
    const double bin = fs / n;
    const std::vector<float> sm = median_smooth(db, bin);
    auto freq = [&](int i) { return (i - n / 2) * bin; };
    // solo dentro del margen util: fuera, el filtro del chip cae y bajaria el suelo
    int lo = n, hi = -1;
    for (int i = 0; i < n; i++)
        if (std::fabs(freq(i)) < edge_hz) {
            lo = std::min(lo, i);
            hi = std::max(hi, i);
        }
    if (hi - lo < 16) return out;
    // suelo: percentil 10 del margen util (una senal de 27 MHz ocupa ~80 % de los 33 MHz)
    std::vector<float> pb(sm.begin() + lo, sm.begin() + hi + 1);
    std::nth_element(pb.begin(), pb.begin() + pb.size() / 10, pb.end());
    const float floor = pb[pb.size() / 10];
    const float thr = floor + kAboveFloorDb;

    // tramos por encima del umbral, uniendo los separados por huecos estrechos
    std::vector<std::pair<int, int>> runs;  // [i, j)
    for (int i = lo; i <= hi;) {
        if (sm[i] <= thr) {
            i++;
            continue;
        }
        int j = i;
        while (j <= hi && sm[j] > thr) j++;
        if (!runs.empty() && (i - runs.back().second) * bin <= kJoinGap) runs.back().second = j;
        else runs.emplace_back(i, j);
        i = j;
    }
    for (const auto &[i, j] : runs) {
        WideRegion r;
        r.f0_hz = freq(i) - bin / 2;
        r.f1_hz = freq(j - 1) + bin / 2;
        if (r.f1_hz - r.f0_hz < kMinVisible) continue;
        r.clip_lo = r.f0_hz <= -edge_hz + kClipMargin;
        r.clip_hi = r.f1_hz >= edge_hz - kClipMargin;
        const std::vector<float> in(sm.begin() + i, sm.begin() + j);
        const float top = median_of(in);
        r.snr_db = top - floor;
        int flat = 0;
        for (float v : in) flat += std::fabs(v - top) <= 3.f;
        r.flat = static_cast<float>(flat) / static_cast<float>(in.size());
        out.push_back(r);
    }
    return out;
}

float band_duty(const cf *x, size_t n, double fs, double f0_hz, double f1_hz) {
    // rodajas de 4096 muestras (~0.1 ms a 40 MS/s), cada una 16 FFT de 256 (Hann); potencia media en la banda
    // (sin el 10 % de cada lado: los bordes caen). Presente = a menos de 6 dB del percentil 95 de las rodajas.
    constexpr int kN = 256, kFrames = 16, kSlice = kN * kFrames;
    const size_t slices = n / kSlice;
    if (slices < 8) return 0;
    const double w = f1_hz - f0_hz;
    const double a = f0_hz + 0.1 * w, b = f1_hz - 0.1 * w;
    std::vector<int> bins;
    for (int k = 0; k < kN; k++) {
        const double f = (k < kN / 2 ? k : k - kN) * fs / kN;  // sin fftshift
        if (f >= a && f <= b) bins.push_back(k);
    }
    if (bins.empty()) return 0;
    std::vector<float> win(kN);
    for (int i = 0; i < kN; i++) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
    std::vector<kp::cf> buf(kN);
    std::vector<double> p(slices, 0);
    for (size_t s = 0; s < slices; s++) {
        for (int fr = 0; fr < kFrames; fr++) {
            const cf *src = x + s * kSlice + static_cast<size_t>(fr) * kN;
            for (int i = 0; i < kN; i++) buf[i] = src[i] * win[i];
            kp::fft_inplace(buf.data(), kN);
            for (int k : bins) p[s] += std::norm(buf[k]);
        }
    }
    std::vector<double> sorted = p;
    std::sort(sorted.begin(), sorted.end());
    const double ref = sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
    size_t on = 0;
    for (double v : p) on += v >= ref / 4;
    return static_cast<float>(on) / static_cast<float>(slices);
}

const std::vector<double> &hdzero_channels_mhz() {
    static const std::vector<double> ch = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917,  // R1-R8
                                           5705, 5740, 5760, 5800,                          // E1 F1 F2 F4
                                           5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}; // L1-L8
    return ch;
}

Kind classify(double centre_hz, double width_hz) {
    if (width_hz < kMinWidth || width_hz > kMaxWidth) return Kind::None;
    bool onChannel = false;
    for (double c : hdzero_channels_mhz()) onChannel = onChannel || std::fabs(centre_hz / 1e6 - c) <= kHdzMaxOffMHz;
    if (onChannel && width_hz >= kHdzWideMin) return Kind::HDZero;
    if (onChannel && width_hz >= kHdzNarrowMin) return Kind::HDZeroNarrow;
    return Kind::Other;
}

const char *kind_code(Kind k) {
    switch (k) {
    case Kind::HDZero: return "hdzero";
    case Kind::HDZeroNarrow: return "hdzero_narrow";
    case Kind::Other: return "digital";
    case Kind::None: break;
    }
    return "";
}

void DigitalScan::reset() {
    cur_.clear();
    prev_.clear();
    havePrev_ = false;
}

void DigitalScan::analyse(const std::vector<float> &db, const cf *x, size_t n, double fs, double edge_hz) {
    edge_ = edge_hz;
    cur_ = find_wide_regions(db, fs, edge_hz);
    for (auto &r : cur_) r.duty = band_duty(x, n, fs, r.f0_hz, r.f1_hz);
}

std::vector<DigitalSignal> DigitalScan::step_done(double centre_hz) {
    std::vector<Abs> now;
    for (const auto &r : cur_) {
        now.push_back(Abs{centre_hz + r.f0_hz, centre_hz + r.f1_hz, r.clip_lo, r.clip_hi, r.snr_db, r.flat, r.duty});
        KP_LOGI("digital: %.1f MHz step, block %.1f..%.1f MHz%s%s, %.1f dB, flat %.2f, duty %.2f", centre_hz / 1e6,
                (centre_hz + r.f0_hz) / 1e6, (centre_hz + r.f1_hz) / 1e6, r.clip_lo ? " [cut low]" : "",
                r.clip_hi ? " [cut high]" : "", r.snr_db, r.flat, r.duty);
    }
    cur_.clear();

    std::vector<Abs> whole;
    for (const auto &a : now)
        if (!a.clip_lo && !a.clip_hi) whole.push_back(a);
    // cortada aqui y en el paso anterior, por lados opuestos, y solapadas: una sola senal
    if (havePrev_ && std::fabs(centre_hz - prevCentre_) <= 2 * edge_) {
        const bool up = centre_hz > prevCentre_;
        for (const auto &a : now)
            for (const auto &p : prev_) {
                const Abs &lo = up ? p : a, &hi = up ? a : p;
                if (!lo.clip_hi || lo.clip_lo || !hi.clip_lo || hi.clip_hi) continue;
                if (std::min(lo.f1, hi.f1) - std::max(lo.f0, hi.f0) < 1e6) continue;
                whole.push_back(Abs{lo.f0, hi.f1, false, false, std::max(lo.snr, hi.snr), std::min(lo.flat, hi.flat),
                                    std::min(lo.duty, hi.duty)});
            }
    }
    prev_ = now;
    prevCentre_ = centre_hz;
    havePrev_ = true;

    std::vector<DigitalSignal> out;
    for (const auto &a : whole) {
        if (a.snr < kMinSnrDb || a.flat < kMinFlat || a.duty < kMinDuty) continue;
        DigitalSignal s;
        s.centre_hz = (a.f0 + a.f1) / 2;
        s.width_hz = a.f1 - a.f0;
        s.snr_db = a.snr;
        s.kind = classify(s.centre_hz, s.width_hz);
        if (s.kind == Kind::None) continue;
        KP_LOGI("digital: %s at %.1f MHz, %.1f MHz wide, %.1f dB", kind_code(s.kind), s.centre_hz / 1e6,
                s.width_hz / 1e6, s.snr_db);
        out.push_back(s);
    }
    return out;
}

}  // namespace kpx
