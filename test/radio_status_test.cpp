// Lectura del estado de fpv::Radio (status_json) para la GUI (src/radio_status.cpp), sin adaptador: un estado real
// copiado de radio_sim y la clave de la lista de pilotos (no cambia con la edad de cada uno).
//   radio_status_test        todos los casos; sale con 1 si alguno falla
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <string>

#include "radio_status.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

QJsonObject json(const char *s) { return QJsonDocument::fromJson(QByteArray(s)).object(); }

// radio_sim --scan con FM en R4 (test/radio_sim.cpp), viendolo
const char kWatch[] =
    R"j({"open":true,"usb3":true,"chip":"SIM (recording)","rx_state":"streaming","rx_error":"","last_fault":"",)j"
    R"j("mode":"watch","freq_hz":5.76938e+09,"watch_hz":5.76938e+09,"fs":4e+07,"banks":19535,"dropped":3,)j"
    R"j("decode_dropped":0,"restarts":2,"dsp_load":0.0984338,"decode_load":0.0864649,"watch_msps":40,)j"
    R"j("fallback":false,"snr_db":27.8533,"video":{"locked":true,"lock":1,"std":"NTSC","fps":59.9,"acquisitions":1,)j"
    R"j("losses":0,"rephases":6,"colour":true},"test":null,"pilots":[{"mhz":5769.38,"name":"R4","std":"NTSC",)j"
    R"j("snr":29.4349,"age_s":16},{"mhz":5843,"name":"R6","snr":20,"age_s":3,"digital":"hdzero","bw":27}]})j";
}  // namespace

int main() {
    const QJsonObject o = json(kWatch);
    const RadioStatus s = radio::parseStatus(o);
    check(s.open && s.usb3 && s.chip == QStringLiteral("SIM (recording)"), "abierto, USB 3, chip");
    check(s.mode == QStringLiteral("watch") && s.rxState == QStringLiteral("streaming"), "modo y estado del flujo");
    check(std::abs(s.watchMHz - 5769.38) < 0.01 && s.msps == 40 && !s.fallback, "viendo 5769,38 MHz a 40 MS/s");
    check(std::abs(s.snrDb - 27.8533) < 1e-3, "SNR");
    check(s.locked && s.lock == 1 && s.videoStd == QStringLiteral("NTSC") && s.colour && s.fps > 59,
          "video: enganchado, NTSC, en color");
    check(s.dropped == 3 && s.restarts == 2, "perdidos y reinicios");
    check(radio::parseStatus(json(R"({"open":false})")).snrDb == -1, "sin snr_db: -1 (sin medida)");

    const QVector<Pilot> ps = radio::parsePilots(o);
    check(ps.size() == 2, "dos pilotos");
    check(ps.size() == 2 && ps[0].name == QStringLiteral("R4") && ps[0].stdName == QStringLiteral("NTSC") &&
              ps[0].ageS == 16 && !ps[0].isDigital(),
          "R4 analogico, NTSC, visto hace 16 s");
    check(ps.size() == 2 && ps[1].digital == QStringLiteral("hdzero") && ps[1].bwMHz == 27, "R6 HDZero, 27 MHz");

    // la edad sube cada segundo: la lista no se rehace por eso, si por un piloto nuevo o una SNR distinta
    QVector<Pilot> older = ps;
    for (Pilot &p : older) p.ageS += 1;
    check(radio::pilotsKey(older) == radio::pilotsKey(ps), "clave: la edad no cuenta");
    QVector<Pilot> other = ps;
    other[0].snrDb += 2;
    check(radio::pilotsKey(other) != radio::pilotsKey(ps), "clave: otra SNR si");

    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
