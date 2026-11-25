// Analizador de 2,4 GHz para ELRS (src/elrs_advisor.cpp) con espectros sinteticos, sin adaptador.
//   elrs_advisor_test        todos los casos; sale con 1 si alguno falla
//   elrs_advisor_test F.csv  lo que dice de una medida de verdad (radio_live --survey 2402 2482 --dump F.csv)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "elrs_advisor.h"

using namespace elrs;

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-64s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// suelo con ruido como el medido con el adaptador (media -0.1..1.5 dB, picos 4-7 dB, duty ~2 %), 2402..2482 MHz
// (lo que mide el barrido de 2,4 GHz), cada 156.25 kHz
std::vector<Bin> noisy() {
    std::vector<Bin> b;
    unsigned s = 4242;
    const auto u = [&s] {
        s = s * 1103515245u + 12345u;
        return static_cast<float>((s >> 8) & 0xffff) / 65535.f;
    };
    for (double f = 2402.078; f < 2482; f += 0.15625) b.push_back(Bin{f, -0.1f + 1.6f * u(), 4.f + 3.f * u(), 0.02f});
    return b;
}

// un AP WiFi de 20 MHz (16.6 ocupados) en el canal ch
void wifi(std::vector<Bin> &b, int ch, float mean, float peak, float duty) {
    const double c = 2412 + 5.0 * (ch - 1);
    for (auto &x : b)
        if (std::fabs(x.mhz - c) < 8.3) {
            x.mean_db = std::max(x.mean_db, mean);
            x.peak_db = std::max(x.peak_db, peak);
            x.duty = std::max(x.duty, duty);
        }
}

bool hasTip(const Report &r, Tip t) { return std::find(r.tips.begin(), r.tips.end(), t) != r.tips.end(); }
bool hasOther(const Report &r, Other k) {
    return std::any_of(r.other.begin(), r.other.end(), [&](const Emission &e) { return e.kind == k; });
}
}  // namespace

// un CSV de radio_live --dump (mhz,mean_db,peak_db,duty)
std::vector<Bin> load(const std::string &path) {
    std::vector<Bin> b;
    if (FILE *f = std::fopen(path.c_str(), "r")) {
        char line[256];
        if (std::fgets(line, sizeof line, f)) {  // cabecera
            Bin x;
            while (std::fscanf(f, "%lf,%f,%f,%f", &x.mhz, &x.mean_db, &x.peak_db, &x.duty) == 4) b.push_back(x);
        }
        std::fclose(f);
    }
    return b;
}

void report(const char *path) {
    const std::vector<Bin> b = load(path);
    const Report r = analyse(b, Use::Racing, Region::Fcc);
    std::printf("%s: %zu bins, verdict %d, %d clean %d busy %d bad, hit %.1f %%, worst peak %.1f dB\n", path, b.size(),
                static_cast<int>(r.verdict), r.clean, r.busy, r.bad, r.hit * 100, r.worst_peak);
    for (const auto &w : r.wifi) std::printf("  wifi %d: peak %.1f mean %.1f duty %.3f\n", w.ch, w.peak_db, w.mean_db, w.duty);
    for (const auto &e : r.other)
        std::printf("  other %d: %.1f-%.1f MHz peak %.1f share %.2f\n", static_cast<int>(e.kind), e.lo_mhz, e.hi_mhz,
                    e.peak_db, e.share);
    std::string row;
    for (const auto &c : r.channels)
        row += !c.measured ? '.' : c.level == Level::Clean ? '-' : c.level == Level::Busy ? 'o' : 'X';
    std::printf("  %s\n  tips:", row.c_str());
    for (const auto t : r.tips) std::printf(" %d", static_cast<int>(t));
    std::printf("\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) report(argv[i]);
        return 0;
    }
    {
        const Report r = analyse(noisy(), Use::Racing, Region::Fcc);
        check(r.measured == 78, "solo ruido: 78 de 80 canales medidos (2400.4 y 2401.4 no)");
        check(!r.channels[0].measured && !r.channels[1].measured && r.channels[2].measured,
              "solo ruido: los dos primeros canales sin medir");
        check(r.wifi.empty() && r.other.empty(), "solo ruido: ni WiFi ni otras emisiones");
        check(r.verdict == Verdict::Clean && r.clean == 78, "solo ruido: banda limpia, todos los canales limpios");
        check(hasTip(r, Tip::RateClean) && !hasTip(r, Tip::DynamicPower), "solo ruido: tasa rapida, sin mas potencia");
    }
    {
        auto b = noisy();
        wifi(b, 6, 10, 30, 0.4f);
        const Report r = analyse(b, Use::Freestyle, Region::Fcc);
        check(r.wifi.size() == 1 && r.wifi[0].ch == 6, "WiFi en el 6: se detecta el 6 (no el 5 ni el 7)");
        check(r.wifi.size() == 1 && r.wifi[0].traffic, "WiFi en el 6: con trafico");
        int under = 0;
        for (const auto &c : r.channels) under += c.wifi == 0 && c.level == Level::Bad;
        check(under >= 15, "WiFi en el 6: sus canales de ELRS (~16) muy ocupados");
        check(r.verdict == Verdict::Moderate, "WiFi en el 6: algo de interferencia");
        check(hasTip(r, Tip::RateModerate) && hasTip(r, Tip::OwnWifi) && hasTip(r, Tip::WifiOnTx),
              "WiFi en el 6: tasa robusta y apagar/alejar la WiFi");
    }
    {
        auto b = noisy();
        wifi(b, 1, 8, 28, 0.5f);
        wifi(b, 6, 10, 30, 0.5f);
        wifi(b, 11, 8, 28, 0.5f);
        const Report r = analyse(b, Use::Racing, Region::EuLbt);
        check(r.wifi.size() == 3, "WiFi en 1, 6 y 11: tres redes");
        check(r.verdict == Verdict::Heavy, "WiFi en 1, 6 y 11: mucha interferencia");
        check(hasTip(r, Tip::RateHeavy) && hasTip(r, Tip::Gemini) && hasTip(r, Tip::LbtBusy),
              "WiFi en 1, 6 y 11: DVDA, Gemini y aviso de LBT");
        check(r.hit > 0.3f && hasTip(r, Tip::Band900), "WiFi en 1, 6 y 11: sugiere 900 MHz");
    }
    {
        auto b = noisy();
        wifi(b, 11, 0.5f, 20, 0.01f);  // AP callado: solo beacons
        const Report r = analyse(b, Use::Racing, Region::Fcc);
        check(r.wifi.size() == 1 && r.wifi[0].ch == 11 && !r.wifi[0].traffic, "AP callado en el 11: solo beacons");
        check(r.verdict != Verdict::Heavy, "AP callado en el 11: no es mucha interferencia");
    }
    {
        auto b = noisy();
        for (auto &x : b)  // Bluetooth: picos cortos en un MHz de cada tres
            if (static_cast<int>(x.mhz) % 3 == 0) x.peak_db = 18, x.duty = 0.03f;
        const Report r = analyse(b, Use::Freestyle, Region::Fcc);
        check(r.wifi.empty(), "Bluetooth: no es WiFi");
        check(hasOther(r, Other::Hoppers) && hasTip(r, Tip::Hoppers), "Bluetooth: emisoras que saltan");
    }
    {
        auto b = noisy();
        for (auto &x : b)  // un emisor de video analogico de 2,4 GHz: continuo, 2446-2454
            if (std::fabs(x.mhz - 2450) < 4) x.mean_db = 20, x.peak_db = 22, x.duty = 1;
        b[100].mean_db = 15, b[100].duty = 1;  // y una espuria de un bin: ni emision ni canal ocupado
        const Report r = analyse(b, Use::LongRange, Region::Fcc);
        check(hasOther(r, Other::Continuous) && hasTip(r, Tip::Continuous), "video 2,4 GHz: emision continua");
        int cont = 0;
        for (const auto &e : r.other) cont += e.kind == Other::Continuous;
        check(cont == 1, "video 2,4 GHz: una sola (la espuria de un bin no cuenta)");
        const auto &c = r.channels[static_cast<size_t>(std::lround((b[100].mhz - kFirstMhz) / kSpacingMhz))];
        check(c.level == Level::Clean, "espuria de un bin: su canal de ELRS sigue limpio");
    }
    {
        auto b = noisy();
        b[200].peak_db = 45;
        const Report r = analyse(b, Use::Racing, Region::Fcc);
        check(hasOther(r, Other::Strong) && hasTip(r, Tip::Strong), "pico de 45 dB: transmisor muy cerca");
    }
    {
        auto b = noisy();
        b.resize(b.size() / 3);  // solo el primer paso del barrido
        const Report r = analyse(b, Use::Racing, Region::Fcc);
        check(r.verdict == Verdict::Unknown && r.tips.empty(), "un tercio medido: aun sin veredicto");
    }
    {
        // ExpressLRS src/src/common.cpp: 500 Hz LoRa 1507 us en el aire cada 2000 us; F500 389 us cada 2000 us
        const Mode &m500 = mode(ModeId::Hz500), &f500 = mode(ModeId::F500), &d250 = mode(ModeId::D250);
        check(std::fabs(pilot_collision(m500) - 1.f / 80) < 1e-6f, "500 Hz: otro piloto pisa 1 de cada 80 envios");
        check(pilot_collision(f500) < 0.5f * pilot_collision(m500), "F500: menos de la mitad de choques que 500 Hz");
        check(std::fabs(packet_loss(m500, 0.2f, 0) - 0.2f) < 1e-6f, "500 Hz sin pilotos: pierde lo ocupado (20 %)");
        check(std::fabs(packet_loss(mode(ModeId::D500), 0.2f, 0) - 0.04f) < 1e-6f, "D500: 2 frecuencias, 20 % -> 4 %");
        check(std::fabs(packet_loss(d250, 0.2f, 0) - 0.008f) < 1e-6f, "D250: 3 frecuencias, 20 % -> 0.8 %");
        const float p7 = packet_loss(m500, 0, 7);
        check(p7 > 0.08f && p7 < 0.09f, "500 Hz con 7 pilotos mas, banda limpia: ~8.4 %");
    }
    {
        const Report r0 = analyse(noisy(), Use::Racing, Region::Fcc, 0);
        const Report r7 = analyse(noisy(), Use::Racing, Region::Fcc, 7);
        check(!hasTip(r0, Tip::Pilots) && hasTip(r7, Tip::Pilots), "7 pilotos: consejo de varios pilotos");
        check(hasTip(r7, Tip::Gemini), "7 pilotos: Gemini (la documentacion lo recomienda con 6-8)");
        check(r0.verdict == Verdict::Clean && r7.verdict == Verdict::Moderate, "7 pilotos en banda limpia: algo de interferencia");
        check(hasTip(r7, Tip::Telemetry) && !r7.modes.empty(), "siempre: telemetria y tabla de modos");
    }
#ifdef KP_TEST_DATA
    {
        // medidas de verdad (RTL8812BU, radio_live --survey 2402 2482 --seconds 40, 2026-11-25): la misma casa con la
        // emisora ELRS apagada y encendida a 250 Hz en otra habitacion
        const auto off = load(std::string(KP_TEST_DATA) + "/elrs_24_off.csv");
        const auto on = load(std::string(KP_TEST_DATA) + "/elrs_24_on_250hz.csv");
        check(off.size() > 400 && on.size() > 400, "medidas reales: cargadas");
        const Report a = analyse(off, Use::Racing, Region::Fcc), b = analyse(on, Use::Racing, Region::Fcc);
        const auto wifiChs = [](const Report &r) {
            std::vector<int> v;
            for (const auto &w : r.wifi) v.push_back(w.ch);
            std::sort(v.begin(), v.end());
            return v;
        };
        check(wifiChs(a) == std::vector<int>{1, 7}, "real, emisora apagada: WiFi en el 1 y en el 7");
        check(a.bad == 0 && a.verdict == Verdict::Clean && !hasOther(a, Other::Hoppers),
              "real, emisora apagada: ningun canal muy ocupado, sin emisoras que saltan");
        check(hasOther(b, Other::Hoppers) && hasTip(b, Tip::Hoppers), "real, ELRS a 250 Hz: emisora que salta");
        // su media sube 5-10 dB (rafagas fuertes y raras), pero ocupa cada canal un 1-3 % del tiempo
        check(b.bad == 0 && b.verdict != Verdict::Heavy, "real, ELRS a 250 Hz: no deja canales muy ocupados");
    }
#endif
    std::printf("\n%s\n", failures ? "FALLAN algunos casos" : "OK");
    return failures ? 1 : 0;
}
