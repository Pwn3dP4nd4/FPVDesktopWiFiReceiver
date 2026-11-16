// Banco del detector de senales digitales (src/kp_ext/digital_scan.cpp) sin adaptador: genera senales sinteticas
// a 40 MS/s y las pasa por el detector paso a paso como el escaneo de kepyur (12 ms de IQ por paso, espectro de
// 1024 puntos, +-16.5 MHz utiles, filtro del chip a +-18 MHz).
//
//   digital_scan_test                      todos los casos; sale con 1 si alguno falla
//   digital_scan_test --write F.cs16 KIND MHZ [SNR]
//       graba 0.5 s de una senal para radio_sim (KP_SIM_FILE=F.cs16, centro 5770 MHz). KIND: hdzero, narrow,
//       wifi, dji, fm, fmnoise
//
// Las senales son modelos, no capturas reales: HDZero se modela como OFDM continuo y plano de 27 (o 17) MHz porque
// es lo que se sabe de el (hoja de datos del DM5680: OFDM; 27 MHz segun Divimath). Con una captura real hay que
// repetir las medidas (Grabar IQ en modo desarrollador).
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "digital_scan.h"
#include "log.h"
#include "spectrum.h"

namespace {
using cf = std::complex<float>;
constexpr double kFs = 40e6;
constexpr double kEdge = 16.5e6;
constexpr size_t kStepSamples = static_cast<size_t>(0.012 * kFs);  // kPsdSpan de kepyur
constexpr double kNoiseSigma = 20;                                    // ruido del LA (cuentas de 12 bits)

std::mt19937 rng(1234);

enum class Sig { None, Ofdm, FmBars, FmNoise };
struct Signal {
    Sig type = Sig::None;
    double mhz = 0;      // frecuencia central absoluta
    double width = 27e6; // OFDM: ancho ocupado
    double snr_db = 15;  // OFDM: densidad espectral sobre el ruido; FM: potencia total sobre el ruido en 20 MHz
    double duty = 1;     // OFDM: fraccion del tiempo emitiendo (rafagas de 0.3 ms)
};

void ifft(std::vector<cf> &x) {
    for (auto &v : x) v = std::conj(v);
    kp::fft_inplace(x.data(), static_cast<int>(x.size()));
    const float k = 1.f / static_cast<float>(x.size());
    for (auto &v : x) v = std::conj(v) * k;
}

// OFDM: FFT de 1024 (39 kHz entre subportadoras), prefijo 1/8, QPSK, sin la subportadora central.
// Subportadoras fuera de +-18 MHz no pasan (filtro del chip).
void add_ofdm(std::vector<cf> &x, double offset, const Signal &s) {
    constexpr int N = 1024, CP = 128;
    const double df = kFs / N;
    const double n0 = kNoiseSigma * kNoiseSigma * 2 / kFs;  // densidad del ruido (I y Q)
    const double psd = n0 * std::pow(10.0, s.snr_db / 10);
    // potencia por subportadora en el dominio del tiempo: |X_k|^2 / N^2 por muestra; densidad = esa / df
    const float amp = static_cast<float>(std::sqrt(psd * df) * N);
    std::uniform_int_distribution<int> bit(0, 1);
    std::vector<cf> sym(N);
    const double burst = 0.3e-3 * kFs;
    double t = std::uniform_real_distribution<double>(0, burst / std::max(0.01, s.duty))(rng);
    for (size_t pos = 0; pos < x.size(); pos += N + CP) {
        const double period = burst / std::max(0.01, s.duty);
        const bool on = s.duty >= 1 || std::fmod(t + pos, period) < burst;
        if (!on) continue;
        std::fill(sym.begin(), sym.end(), cf(0, 0));
        for (int k = -N / 2; k < N / 2; k++) {
            const double f = offset + k * df;
            if (k == 0 || std::fabs(k * df) > s.width / 2 || std::fabs(f) > 18e6) continue;
            // la subportadora k de la senal cae en la k + offset/df del receptor
            int bin = static_cast<int>(std::lround(f / df));
            if (bin < -N / 2 || bin >= N / 2) continue;
            const float re = bit(rng) ? amp : -amp, im = bit(rng) ? amp : -amp;
            sym[(bin + N) % N] = cf(re, im) * static_cast<float>(M_SQRT1_2);
        }
        ifft(sym);
        for (int i = 0; i < N + CP && pos + i < x.size(); i++) x[pos + i] += sym[(i - CP + N) % N];
    }
}

// FM con video analogico NTSC: linea de 63.5 us (sincronismo 4.7 us a -40 IRE, negro 0, imagen 52.6 us),
// desviacion de ~8 MHz de punta de sincronismo a blanco (lo de un VTX analogico tipico).
void add_fm(std::vector<cf> &x, double offset, const Signal &s, bool noiseVideo) {
    const double line = 63.5e-6 * kFs, sync = 4.7e-6 * kFs, active0 = 10.9e-6 * kFs;
    const double devPerUnit = 8e6 / 1.4;  // -0.4 .. 1.0
    const double p = kNoiseSigma * kNoiseSigma * 2 * 0.5 * std::pow(10.0, s.snr_db / 10);  // 20 de 40 MHz
    const float a = static_cast<float>(std::sqrt(p));
    std::uniform_real_distribution<double> u(0, 1);
    double ph = 0, pix = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double tl = std::fmod(static_cast<double>(i), line);
        double v;
        if (tl < sync) v = -0.4;
        else if (tl < active0) v = 0;
        else if (noiseVideo) {
            if (static_cast<int>(tl) % 8 == 0) pix = u(rng);  // un "pixel" cada 0.2 us: imagen muy cargada
            v = pix;
        } else {
            v = std::floor((tl - active0) / ((line - active0) / 8)) / 7.0;  // 8 barras
        }
        const double f = offset + (v - 0.3) * devPerUnit;
        ph += 2 * M_PI * f / kFs;
        x[i] += a * cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
}

std::vector<cf> step_samples(double centre_hz, const std::vector<Signal> &sigs, size_t n = kStepSamples) {
    std::normal_distribution<float> nd(0, static_cast<float>(kNoiseSigma));
    std::vector<cf> x(n);
    for (auto &v : x) v = cf(nd(rng), nd(rng));
    for (const auto &s : sigs) {
        const double off = s.mhz * 1e6 - centre_hz;
        if (s.type == Sig::Ofdm) {
            if (std::fabs(off) - s.width / 2 < 18e6) add_ofdm(x, off, s);
        } else if (s.type == Sig::FmBars || s.type == Sig::FmNoise) {
            if (std::fabs(off) < 12e6) add_fm(x, off, s, s.type == Sig::FmNoise);
        }
    }
    return x;
}

std::vector<double> full_plan(double from, double to) {
    std::vector<double> p;
    for (double c = 5652e6; c <= 5940e6; c += 20e6)
        if (c >= from - 40e6 && c <= to + 40e6) p.push_back(c);
    return p;
}

std::vector<kpx::DigitalSignal> run(const std::vector<double> &plan, const std::vector<Signal> &sigs) {
    kpx::DigitalScan ds;
    ds.reset();
    std::vector<kpx::DigitalSignal> all;
    for (double c : plan) {
        const auto x = step_samples(c, sigs);
        kp::Psd psd(1024);
        psd.add(x.data(), x.size());
        ds.analyse(psd.db(), x.data(), x.size(), kFs, kEdge);
        for (const auto &d : ds.step_done(c)) {
            bool dup = false;
            for (const auto &e : all) dup = dup || std::fabs(e.centre_hz - d.centre_hz) < 4e6;
            if (!dup) all.push_back(d);
        }
    }
    return all;
}

int failures = 0;

void expect(const char *name, const std::vector<kpx::DigitalSignal> &got, kpx::Kind kind, double mhz,
            double minW = 0, double maxW = 1e9) {
    bool ok;
    if (kind == kpx::Kind::None) {
        ok = got.empty();
    } else {
        ok = got.size() == 1 && got[0].kind == kind && std::fabs(got[0].centre_hz / 1e6 - mhz) <= 1.0 &&
             got[0].width_hz >= minW && got[0].width_hz <= maxW;
    }
    std::printf("%-44s %s", name, ok ? "ok  " : "FAIL");
    if (got.empty()) std::printf("  (nada)");
    for (const auto &d : got)
        std::printf("  [%s %.2f MHz, %.1f MHz, %.1f dB]", kpx::kind_code(d.kind), d.centre_hz / 1e6, d.width_hz / 1e6,
                    d.snr_db);
    std::printf("\n");
    if (!ok) failures++;
}

int write_recording(const char *file, const std::string &kind, double mhz, double snr) {
    Signal s;
    s.mhz = mhz;
    s.snr_db = snr;
    if (kind == "hdzero") s.type = Sig::Ofdm;
    else if (kind == "narrow") { s.type = Sig::Ofdm; s.width = 17e6; }
    else if (kind == "wifi") { s.type = Sig::Ofdm; s.width = 16.25e6; s.duty = 0.3; }
    else if (kind == "dji") { s.type = Sig::Ofdm; s.width = 18e6; }
    else if (kind == "fm") { s.type = Sig::FmBars; s.snr_db = snr + 10; }
    else if (kind == "fmnoise") { s.type = Sig::FmNoise; s.snr_db = snr + 10; }
    else { std::fprintf(stderr, "tipo desconocido: %s\n", kind.c_str()); return 2; }
    const auto x = step_samples(5770e6, {s}, static_cast<size_t>(0.5 * kFs));
    FILE *f = std::fopen(file, "wb");
    if (!f) { std::fprintf(stderr, "no puedo crear %s\n", file); return 1; }
    std::vector<int16_t> buf(2 * x.size());
    for (size_t i = 0; i < x.size(); i++) {
        auto q = [](float v) { return static_cast<int16_t>(std::lround(std::max(-2047.f, std::min(2047.f, v))) * 16); };
        buf[2 * i] = q(x[i].real());
        buf[2 * i + 1] = q(x[i].imag());
    }
    std::fwrite(buf.data(), 2, buf.size(), f);
    std::fclose(f);
    std::printf("%s: 0.5 s, %s a %.1f MHz (centro 5770 MHz, 40 MS/s, cs16)\n", file, kind.c_str(), mhz);
    return 0;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--write") == 0)
        return write_recording(argv[2], argv[3], std::atof(argv[4]), argc >= 6 ? std::atof(argv[5]) : 15);
    kp::set_log_debug(false);
    using K = kpx::Kind;
    const double R[] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};

    // HDZero (27 MHz) en cada canal R con el barrido completo de kepyur (pasos cada 20 MHz): casi siempre cortado
    // entre dos pasos, hay que unirlo
    for (int i = 0; i < 8; i++) {
        const Signal s{Sig::Ofdm, R[i], 27e6, 15, 1};
        char name[64];
        std::snprintf(name, sizeof name, "HDZero 27 MHz R%d, barrido completo", i + 1);
        expect(name, run(full_plan(R[i] * 1e6, R[i] * 1e6), {s}), K::HDZero, R[i], 25e6, 29e6);
    }
    // centrado en el paso (plan de una seleccion de canales R: un paso por canal)
    expect("HDZero 27 MHz R3, paso centrado", run({5732e6}, {{Sig::Ofdm, 5732, 27e6, 15, 1}}), K::HDZero, 5732);
    expect("HDZero 27 MHz F1, barrido completo", run(full_plan(5740e6, 5740e6), {{Sig::Ofdm, 5740, 27e6, 15, 1}}),
           K::HDZero, 5740);
    expect("HDZero 27 MHz R5 debil (9 dB)", run(full_plan(5806e6, 5806e6), {{Sig::Ofdm, 5806, 27e6, 9, 1}}),
           K::HDZero, 5806);
    expect("HDZero 27 MHz R5 muy debil (5 dB): no", run(full_plan(5806e6, 5806e6), {{Sig::Ofdm, 5806, 27e6, 5, 1}}),
           K::None, 0);
    expect("HDZero 27 MHz R4 fuerte (35 dB)", run(full_plan(5769e6, 5769e6), {{Sig::Ofdm, 5769, 27e6, 35, 1}}),
           K::HDZero, 5769);
    // estrecho (17 MHz, 540p60)
    expect("HDZero 17 MHz R4", run(full_plan(5769e6, 5769e6), {{Sig::Ofdm, 5769, 17e6, 15, 1}}), K::HDZeroNarrow,
           5769, 15e6, 19e6);
    expect("HDZero 17 MHz R7", run(full_plan(5880e6, 5880e6), {{Sig::Ofdm, 5880, 17e6, 15, 1}}), K::HDZeroNarrow,
           5880);
    // dos HDZero a la vez (R2 y R6)
    {
        const auto got = run(full_plan(5695e6, 5843e6), {{Sig::Ofdm, 5695, 27e6, 15, 1}, {Sig::Ofdm, 5843, 27e6, 12, 1}});
        const bool ok = got.size() == 2;
        std::printf("%-44s %s  (%zu senales)\n", "HDZero R2 + R6 a la vez", ok ? "ok  " : "FAIL", got.size());
        if (!ok) failures++;
    }
    // otras senales digitales: no son HDZero
    expect("OFDM continuo 18 MHz en 5745 (no HDZero)", run(full_plan(5745e6, 5745e6), {{Sig::Ofdm, 5745, 18e6, 15, 1}}),
           K::Other, 5745);
    expect("WiFi 20 MHz a rafagas (30 %) en 5745: no", run(full_plan(5745e6, 5745e6), {{Sig::Ofdm, 5745, 16.25e6, 20, 0.3}}),
           K::None, 0);
    expect("WiFi 40 MHz continuo en 5775: no", run(full_plan(5775e6, 5775e6), {{Sig::Ofdm, 5775, 38e6, 20, 1}}),
           K::None, 0);
    // video analogico (FM): nunca digital
    expect("FM analogico barras R4, fuerte: no", run(full_plan(5769e6, 5769e6), {{Sig::FmBars, 5769, 0, 30, 1}}),
           K::None, 0);
    expect("FM analogico imagen cargada R5: no", run(full_plan(5806e6, 5806e6), {{Sig::FmNoise, 5806, 0, 30, 1}}),
           K::None, 0);
    expect("FM analogico imagen cargada R3, centrado: no", run({5732e6}, {{Sig::FmNoise, 5732, 0, 35, 1}}), K::None, 0);
    expect("solo ruido: no", run(full_plan(5652e6, 5940e6), {}), K::None, 0);

    std::printf(failures ? "\n%d caso(s) FALLAN\n" : "\ntodo bien\n", failures);
    return failures ? 1 : 0;
}
