// Tarjeta de un piloto en la lista de la ventana principal (src/pilot_card.h): sus dos renglones, el nivel de su SNR y
// cuanto se llena su barra. Sin ventana ni adaptador.
//   pilot_card_test        sale con 1 si algun caso falla
#include <cstdio>
#include <string>

#include "pilot_card.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

void checkText(const QString &got, const QString &want, const std::string &what) {
    check(got == want, what);
    if (got != want) std::printf("    da:     \"%s\"\n    espera: \"%s\"\n", qPrintable(got), qPrintable(want));
}

Pilot analog(double mhz, const QString &std, double snr) {
    Pilot p;
    p.freqMHz = mhz;
    p.stdName = std;
    p.snrDb = snr;
    return p;
}

void linesCases() {
    const QString dot = QStringLiteral(" · ");
    pilotcard::Lines l = pilotcard::lines(analog(5769, QStringLiteral("NTSC"), 18.5), QStringLiteral("R4/A6/B3"));
    checkText(l.title, QStringLiteral("R4/A6/B3"), "analogico con canal: titulo, el canal");
    checkText(l.detail, QStringLiteral("5769 MHz") + dot + QStringLiteral("NTSC"), "analogico con canal: MHz y norma");

    l = pilotcard::lines(analog(5701.4, QStringLiteral("PAL"), 9), QString());
    checkText(l.title, QStringLiteral("5701 MHz"), "sin canal cerca: titulo, los MHz");
    checkText(l.detail, QStringLiteral("PAL"), "sin canal cerca: solo la norma");

    l = pilotcard::lines(analog(5806, QString(), 11), QStringLiteral("R5"));
    checkText(l.detail, QStringLiteral("5806 MHz"), "sin norma aun: solo los MHz");

    Pilot d = analog(5760, QString(), 20);
    d.digital = QStringLiteral("hdzero");
    d.name = QStringLiteral("F2");
    l = pilotcard::lines(d, QStringLiteral("R4"));  // digital: el canal HDZero (name), no el analogico
    checkText(l.title, QStringLiteral("F2"), "digital: titulo, su canal HDZero");
    checkText(l.detail, QStringLiteral("5760 MHz") + dot + QStringLiteral("HDZero"), "digital: MHz y tipo");
    d.name.clear();
    checkText(pilotcard::lines(d, QString()).title, QStringLiteral("5760 MHz"), "digital sin canal: los MHz");
}

void snrCases() {
    using pilotcard::Snr;
    check(pilotcard::snrLevel(kLowSnrDb - 0.1) == Snr::Poor, "SNR por debajo de kLowSnrDb: flojo");
    check(pilotcard::snrLevel(kLowSnrDb) == Snr::Fair, "SNR en kLowSnrDb: regular");
    check(pilotcard::snrLevel(pilotcard::kGoodSnrDb - 0.1) == Snr::Fair, "SNR por debajo de kGoodSnrDb: regular");
    check(pilotcard::snrLevel(pilotcard::kGoodSnrDb) == Snr::Good, "SNR en kGoodSnrDb: bueno");
    check(pilotcard::snrFill(-3) == 0, "barra: SNR negativa, vacia");
    check(pilotcard::snrFill(pilotcard::kFullSnrDb / 2) == 0.5, "barra: media escala, mitad");
    check(pilotcard::snrFill(pilotcard::kFullSnrDb + 10) == 1, "barra: por encima de la escala, llena");
}
}  // namespace

int main() {
    linesCases();
    snrCases();
    std::printf("\n%s (%d fallo(s))\n", failures ? "FALLA" : "OK", failures);
    return failures ? 1 : 0;
}
