// Logica de la ventana del analizador (src/channel_analyzer.cpp) con un backend falso, sin pantalla ni adaptador:
//   analyzer_logic_test        todos los casos; sale con 1 si alguno falla
// Lo que no cubren race_monitor_test ni channel_advisor_test: el ritmo de los barridos (el backend real manda el
// espectro cada segundo y un barrido tarda ~3 s), parar y volver a empezar con la respuesta del backend en cola, y el
// auto-inicio y fin automatico de la carrera con su aviso. Usa su propio .ini (carpeta temporal).
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <string>

#include "channel_analyzer.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-72s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// Backend sin radio: recuerda las ordenes. stopSurvey responde en cola (como el real, en su hilo) si queued
class FakeBackend : public Backend {
public:
    bool queued = true;
    int starts = 0, stops = 0;
    QString name() const override { return QStringLiteral("fake"); }
    void openDevice() override {}
    void closeDevice() override {}
    void startScan() override {}
    void setScanChannels(const QVector<double> &) override {}
    void watch(double) override {}
    void stop() override {}
    void startSurvey(double, double) override { starts++; }
    void stopSurvey() override {
        stops++;
        if (queued) QTimer::singleShot(0, this, [this] { emit surveyStopped(); });
        else emit surveyStopped();
    }
};

const double kR1 = 5658, kR3 = 5732, kR6 = 5843, kR8 = 5917;

Pilot pilot(double mhz, double snr = 25) {
    Pilot p;
    p.freqMHz = mhz;
    p.snrDb = snr;
    p.ageS = 0;
    return p;
}

// un espectro llano con estos pilotos encima
SurveyData survey(int sweeps, const QVector<Pilot> &pilots) {
    SurveyData d;
    d.sweeps = sweeps;
    for (double f = 5600.078; f < 5960; f += 0.15625) {
        SpectrumPoint b{f, 0.5f, 3.f, 0.f};
        for (const Pilot &p : pilots)
            if (std::abs(f - p.freqMHz) < 8) b = SpectrumPoint{f, 25.f, 25.f, 1.f};
        d.bins.push_back(b);
    }
    d.pilots = pilots;
    return d;
}

void open(FakeBackend &b) {
    RadioStatus s;
    s.open = true;
    emit b.statusChanged(s);
}
}  // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir ini;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, ini.path());
    QApplication app(argc, argv);

    {
        // ritmo: el espectro llega cada segundo, a medio barrido; el seguimiento y la recomendacion van por barrido
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        w.setCounts(4, 0);
        w.setPinnedForTest({QStringLiteral("R1"), QStringLiteral("R3"), QStringLiteral("R6"), QStringLiteral("R8")});
        w.startForTest();
        w.setRaceForTest(true);
        const QVector<Pilot> four{pilot(kR1), pilot(kR3), pilot(kR6), pilot(kR8)};
        const int r0 = w.recomputesForTest();
        for (int k = 0; k < 9; k++) emit b.surveyUpdated(survey(k / 3, four));  // 3 barridos, 3 veces cada uno
        check(w.raceOnForTest() && w.raceSweepsForTest() == 3,
              "9 actualizaciones de 3 barridos: el seguimiento cuenta 3 barridos");
        check(w.recomputesForTest() - r0 == 3, "la recomendacion se recalcula una vez por barrido");
        QVector<Pilot> more = four;
        more.push_back(pilot(5769));  // alguien sale en R4 a medio barrido: se recomienda de nuevo
        emit b.surveyUpdated(survey(2, more));
        check(w.recomputesForTest() - r0 == 4 && w.raceSweepsForTest() == 3,
              "un piloto nuevo a medio barrido: se recomienda de nuevo, sin contar otro barrido");
    }
    {
        // parar y volver a empezar antes de que llegue la respuesta del backend: sigue en marcha
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        w.startForTest();
        emit b.surveyUpdated(survey(0, {}));
        w.close();  // stop(): requestStop; su surveyStopped llega en cola
        w.startForTest();
        QCoreApplication::processEvents();
        check(b.stops == 1 && b.starts == 2 && w.runningForTest(),
              "Parar y Empezar seguidos: la respuesta del primer Parar no para el segundo");
        emit b.surveyStopped();  // otra orden (escanear, ver un canal) lo para: eso si
        check(!w.runningForTest(), "una parada que no ha pedido la ventana si la para");
        b.queued = false;  // respuesta inmediata (backend en el mismo hilo)
        w.startForTest();
        w.close();
        w.startForTest();
        check(w.runningForTest(), "con respuesta inmediata tambien");
    }
    {
        // auto-inicio, aviso de que una senal sola impide el fin automatico, y fin automatico
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        w.setCounts(2, 0);
        w.setPinnedForTest({QStringLiteral("R1"), QStringLiteral("R3")});
        w.setTimeForTest(0);
        w.startForTest();
        emit b.surveyUpdated(survey(0, {}));
        w.armForTest(0);  // los dos
        check(w.armedForTest() && !w.raceOnForTest(), "armado: espera a los pilotos");
        double t = 1;
        int sw = 1;
        const auto feed = [&](const QVector<Pilot> &ps, double seconds) {
            for (const double end = t + seconds; t < end; t += 1) {
                w.setTimeForTest(t);
                emit b.surveyUpdated(survey(sw, ps));
                if (static_cast<int>(t) % 3 == 0) sw++;
            }
        };
        feed({pilot(kR1)}, 5);
        check(w.armedForTest() && !w.raceOnForTest(), "uno de dos en el aire: sigue esperando");
        feed({pilot(kR1), pilot(kR3)}, 30);
        check(w.raceOnForTest() && !w.armedForTest(), "los dos en el aire: la carrera empieza sola");
        feed({pilot(kR3)}, 90);  // R1 aterriza; en R3 sigue algo (un VTX ajeno, encendido en la mesa...)
        const QStringList h = w.historyForTest();
        check(w.raceOnForTest() && h.contains(QStringLiteral("Still on the air: the race will not stop by itself")),
              "uno aterrizado y una senal que sigue: no para sola y lo avisa");
        check(h.count(QStringLiteral("Still on the air: the race will not stop by itself")) == 1,
              "el aviso sale una vez");
        feed({}, 70);
        check(!w.raceOnForTest() && w.historyForTest().contains(QStringLiteral("Race stopped")),
              "nadie en el aire durante mas de un minuto: para sola");
    }

    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
