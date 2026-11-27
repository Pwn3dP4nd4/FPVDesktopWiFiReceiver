// Ventana del analizador de 2,4 GHz (src/elrs_analyzer.cpp) sin adaptador ni pantalla: que lo mostrado no salte a cada
// barrido (veredicto con espera, sugerencias que siguen un rato, sugerencias sin rehacer por sus cifras).
//   elrs_window_test        sale con 1 si algun caso falla (sin pantalla: offscreen)
#include <QApplication>
#include <QDir>
#include <QSettings>

#include <cmath>
#include <cstdio>
#include <string>

#include "analyzer_driver.h"
#include "elrs_analyzer.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-66s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// backend que no hace nada: los barridos los pone el test (emit surveyUpdated), uno por ventana
class NullBackend : public Backend {
public:
    QString name() const override { return QStringLiteral("null"); }
    void openDevice() override {}
    void closeDevice() override {}
    void startScan() override {}
    void setScanChannels(const QVector<double> &) override {}
    void watch(double) override {}
    void stop() override {}
};

// 2402..2482 MHz con ruido; heavy: WiFi con trafico en 1, 6 y 11; wifiCh: un AP fuerte en ese canal; jitter: cifras
SurveyData band(int sweep, bool heavy, int wifiCh = 0, float jitter = 0) {
    SurveyData d;
    d.sweeps = sweep;
    unsigned s = 77;
    for (double f = 2402.078; f < 2482; f += 0.15625) {
        s = s * 1103515245u + 12345u;
        const float u = static_cast<float>((s >> 8) & 0xffff) / 65535.f;
        SpectrumPoint p{f, 0.5f * u + jitter, 4.f + 3.f * u, 0.02f, quint32(sweep + 1)};
        for (int ch : {1, 6, 11})
            if (heavy && std::fabs(f - (2412 + 5.0 * (ch - 1))) < 8.3) p.meanDb = 8, p.peakDb = 28, p.duty = 0.5f;
        if (wifiCh && std::fabs(f - (2412 + 5.0 * (wifiCh - 1))) < 8.3) p.meanDb = 5, p.peakDb = 25, p.duty = 0.06f;
        d.bins.push_back(p);
    }
    return d;
}
}  // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath() + QStringLiteral("/elrs_window_test"));
    {
        QSettings st(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("FPVDesktopWiFiReceiver"),
                     QStringLiteral("FPVDesktopWiFiReceiver"));
        st.setValue("elrs/use", 0);
        st.setValue("elrs/region", 0);
        st.setValue("elrs/pilots", 0);
        st.setValue("elrs/memory", 20);
        st.setValue("elrs/autoStop", false);
    }
    qRegisterMetaType<SurveyData>();
    NullBackend backend_w;
    ElrsAnalyzer w(&backend_w);
    w.setAdapterOpen(true);
    w.setTimeForTest(0);
    drive::start(w);
    int sw = 0;
    const auto at = [&](double t, const SurveyData &d) {
        w.setTimeForTest(t);
        emit backend_w.surveyUpdated(d);
    };

    at(0.5, band(sw++, false));
    check(drive::shows(w, QStringLiteral("Clean band")), "banda limpia: se muestra limpia en seguida");
    at(1, band(sw++, true));
    check(drive::shows(w, QStringLiteral("Clean band")), "un barrido con mucha interferencia: sigue limpia");
    at(1.5, band(sw++, false));
    at(5, band(sw++, true));
    check(drive::shows(w, QStringLiteral("Clean band")), "otra vez, 3.5 s despues: aun limpia");
    for (double t = 5.5; t < 13; t += 0.5) at(t, band(sw++, true));
    check(drive::shows(w, QStringLiteral("Clean band")), "mucha interferencia 7.5 s: aun no (espera 8 s)");
    at(13.5, band(sw++, true));
    check(drive::shows(w, QStringLiteral("Heavy interference")), "mucha interferencia 8.5 s: ya se muestra");

    // las cifras cambian a cada barrido, las sugerencias no: el texto no se rehace en 5 s
    for (double t = 14; t < 30; t += 0.5) at(t, band(sw++, true));
    const QString before = drive::tips(w);
    at(30.5, band(sw++, true, 0, 0.4f));
    at(31, band(sw++, true, 0, 0.8f));
    check(drive::tips(w) == before, "mismas sugerencias, cifras distintas: el texto no se rehace en seguida");

    // un AP fuerte que se ve y deja de verse: su consejo sigue un rato
    for (double t = 40; t < 50; t += 0.5) at(t, band(sw++, false, 3));
    check(drive::tips(w).contains(QStringLiteral("strong network")), "AP fuerte en el 3: consejo de la WiFi");
    for (double t = 50; t < 58; t += 0.5) at(t, band(sw++, false));
    check(drive::tips(w).contains(QStringLiteral("strong network")), "el AP calla 8 s: el consejo sigue");
    for (double t = 58; t < 66; t += 0.5) at(t, band(sw++, false));
    check(!drive::tips(w).contains(QStringLiteral("strong network")), "el AP calla 16 s: el consejo ya no esta");

    // parar solo: banda estable, 20 barridos en la media, al menos 30 s y 15 s sin cambios
    {
        NullBackend backend_a;
        ElrsAnalyzer a(&backend_a);
        a.setAdapterOpen(true);
        drive::setChecked(drive::find<QCheckBox>(&a, "autoStop"), true);
        a.setTimeForTest(0);
        drive::start(a);
        int k = 0;
        double t = 0.5;
        for (; t < 29.9 && a.isRunning(); t += 0.5) {
            a.setTimeForTest(t);
            emit backend_a.surveyUpdated(band(k++, false));
        }
        check(a.isRunning(), "parar solo: no antes de 30 s");
        for (; t < 40 && a.isRunning(); t += 0.5) {
            a.setTimeForTest(t);
            emit backend_a.surveyUpdated(band(k++, false));
        }
        check(!a.isRunning() && t <= 31, "parar solo: banda estable, para a los 30 s");

        NullBackend backend_b;
        ElrsAnalyzer b(&backend_b);
        b.setAdapterOpen(true);
        drive::setChecked(drive::find<QCheckBox>(&b, "autoStop"), true);
        b.setTimeForTest(0);
        drive::start(b);
        k = 0;
        // la banda pasa de limpia a muy ocupada cada 10 s: el veredicto cambia (con su espera de 8 s), no para
        for (t = 0.5; t < 60 && b.isRunning(); t += 0.5) {
            b.setTimeForTest(t);
            emit backend_b.surveyUpdated(band(k++, (int(t) / 10) % 2 == 1));
        }
        check(b.isRunning(), "parar solo: con algo que cambia, sigue midiendo");
        for (; t < 125 && b.isRunning(); t += 0.5) {
            b.setTimeForTest(t);
            emit backend_b.surveyUpdated(band(k++, (int(t) / 10) % 2 == 1));
        }
        check(!b.isRunning() && t <= 121, "parar solo: cambiando sin parar, para a los 120 s");

        // una WiFi debil que se ve 5 s de cada 30 (fila de "En el aire" que va y viene): para igual a los 30 s
        NullBackend backend_e;
        ElrsAnalyzer e(&backend_e);
        e.setAdapterOpen(true);
        drive::setChecked(drive::find<QCheckBox>(&e, "autoStop"), true);
        e.setTimeForTest(0);
        drive::start(e);
        k = 0;
        for (t = 0.5; t < 60 && e.isRunning(); t += 0.5) {
            e.setTimeForTest(t);
            SurveyData d = band(k++, false);
            if (int(t) % 30 < 5)
                for (auto &p : d.bins)
                    if (std::fabs(p.mhz - 2442) < 8.3) p.peakDb = 14, p.duty = 0.03f;  // beacons, sin trafico
            emit backend_e.surveyUpdated(d);
        }
        check(!e.isRunning() && t <= 31, "parar solo: una fila que va y viene no lo impide");

        NullBackend backend_c;
        ElrsAnalyzer c(&backend_c);
        c.setAdapterOpen(true);
        drive::setChecked(drive::find<QCheckBox>(&c, "autoStop"), false);
        c.setTimeForTest(0);
        drive::start(c);
        k = 0;
        for (t = 0.5; t < 60; t += 0.5) {
            c.setTimeForTest(t);
            emit backend_c.surveyUpdated(band(k++, false));
        }
        check(c.isRunning(), "parar solo desactivado: sigue midiendo");
    }
    std::printf("\n%s\n", failures ? "FALLAN algunos casos" : "OK");
    return failures ? 1 : 0;
}
