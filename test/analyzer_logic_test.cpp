// Logica de la ventana del analizador (src/channel_analyzer.cpp) con un backend falso, sin pantalla ni adaptador:
//   analyzer_logic_test        todos los casos; sale con 1 si alguno falla
// Lo que no cubren race_monitor_test ni channel_advisor_test: el ritmo de los barridos (el backend real manda el
// espectro cada segundo y un barrido tarda ~3 s), parar y volver a empezar con la respuesta del backend en cola, y el
// auto-inicio y fin automatico de la carrera con su aviso. Usa su propio .ini (carpeta temporal).
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <cstdio>
#include <string>

#include "analyzer_driver.h"

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
        drive::setCounts(w, 4, 0);
        drive::setPinned(w, {QStringLiteral("R1"), QStringLiteral("R3"), QStringLiteral("R6"), QStringLiteral("R8")});
        drive::start(w);
        drive::setRace(w, true);
        const QVector<Pilot> four{pilot(kR1), pilot(kR3), pilot(kR6), pilot(kR8)};
        const int r0 = w.recomputesForTest();
        for (int k = 0; k < 9; k++) emit b.surveyUpdated(survey(k / 3, four));  // 3 barridos, 3 veces cada uno
        check(drive::raceOn(w) && drive::raceSweeps(w) == 3,
              "9 actualizaciones de 3 barridos: el seguimiento cuenta 3 barridos");
        check(w.recomputesForTest() - r0 == 3, "la recomendacion se recalcula una vez por barrido");
        QVector<Pilot> more = four;
        more.push_back(pilot(5769));  // alguien sale en R4 a medio barrido: se recomienda de nuevo
        emit b.surveyUpdated(survey(2, more));
        check(w.recomputesForTest() - r0 == 4 && drive::raceSweeps(w) == 3,
              "un piloto nuevo a medio barrido: se recomienda de nuevo, sin contar otro barrido");
    }
    {
        // parar y volver a empezar antes de que llegue la respuesta del backend: sigue en marcha
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::start(w);
        emit b.surveyUpdated(survey(0, {}));
        w.close();  // stop(): requestStop; su surveyStopped llega en cola
        drive::start(w);
        QCoreApplication::processEvents();
        check(b.stops == 1 && b.starts == 2 && w.isRunning(),
              "Parar y Empezar seguidos: la respuesta del primer Parar no para el segundo");
        emit b.surveyUpdated(survey(0, {}));  // ya barre para esta ventana
        emit b.surveyStopped();  // otra orden (escanear, ver un canal) lo para: eso si
        check(!w.isRunning(), "una parada que no ha pedido la ventana si la para");
        b.queued = false;  // respuesta inmediata (backend en el mismo hilo)
        drive::start(w);
        w.close();
        drive::start(w);
        check(w.isRunning(), "con respuesta inmediata tambien");
    }
    {
        // el analizador de 2,4 GHz para (su surveyStopped va en cola) y este empieza antes de que llegue: esa
        // respuesta no es para esta ventana, que aun no ha recibido ningun barrido suyo
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::start(w);
        emit b.surveyStopped();  // la respuesta al Parar de la otra ventana
        check(w.isRunning(), "la parada de la otra ventana, antes del primer barrido: sigue en marcha");
    }
    {
        // auto-inicio, aviso de que una senal sola impide el fin automatico, y fin automatico
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::setCounts(w, 2, 0);
        drive::setPinned(w, {QStringLiteral("R1"), QStringLiteral("R3")});
        w.setTimeForTest(0);
        drive::start(w);
        emit b.surveyUpdated(survey(0, {}));
        drive::arm(w, 0);  // los dos
        check(drive::armed(w) && !drive::raceOn(w), "armado: espera a los pilotos");
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
        check(drive::armed(w) && !drive::raceOn(w), "uno de dos en el aire: sigue esperando");
        feed({pilot(kR1), pilot(kR3)}, 30);
        check(drive::raceOn(w) && !drive::armed(w), "los dos en el aire: la carrera empieza sola");
        feed({pilot(kR3)}, 90);  // R1 aterriza; en R3 sigue algo (un VTX ajeno, encendido en la mesa...)
        const QStringList h = drive::history(w);
        check(drive::raceOn(w) && h.contains(QStringLiteral("Still on the air: the race will not stop by itself")),
              "uno aterrizado y una senal que sigue: no para sola y lo avisa");
        check(h.count(QStringLiteral("Still on the air: the race will not stop by itself")) == 1,
              "el aviso sale una vez");
        feed({}, 70);
        check(!drive::raceOn(w) && drive::history(w).contains(QStringLiteral("Race stopped")),
              "nadie en el aire durante mas de un minuto: para sola");
    }

    {
        // barra de barrido: lo que se ha vuelto a medir desde la actualizacion anterior (seq cambia) es donde va
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::start(w);
        const auto part = [](int sweeps, double lo, double hi, quint32 seqIn, quint32 seqOut) {
            SurveyData d = survey(sweeps, {});
            for (auto &x : d.bins) x.seq = x.mhz >= lo && x.mhz < hi ? seqIn : seqOut;
            return d;
        };
        emit b.surveyUpdated(part(0, 5600, 5700, 1, 0));  // primer tercio medido, el resto aun no
        const QVector<int> h1 = drive::view(w)->sweepHeadForTest();
        check(!h1.isEmpty() && *std::min_element(h1.begin(), h1.end()) >= 5600 &&
                  *std::max_element(h1.begin(), h1.end()) < 5700 && drive::view(w)->sweepCellsForTest() == 100,
              "barrido: lo recien medido (5600-5700) y nada mas");
        SurveyData d2 = part(0, 5700, 5800, 2, 1);
        for (auto &x : d2.bins)
            if (x.mhz >= 5800) x.seq = 0;
        emit b.surveyUpdated(d2);
        const QVector<int> h2 = drive::view(w)->sweepHeadForTest();
        check(!h2.isEmpty() && *std::min_element(h2.begin(), h2.end()) >= 5700 &&
                  *std::max_element(h2.begin(), h2.end()) < 5800,
              "barrido: avanza a 5700-5800 (lo de antes no cuenta como nuevo)");
        w.close();
        drive::start(w);
        check(drive::view(w)->sweepCellsForTest() == 0, "barrido: al volver a empezar se olvida lo de antes");
    }
    {
        // historial por piloto: un intruso encima de R3 durante la carrera; R1 limpio. R3 sale el primero, con graves
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::setCounts(w, 2, 0);
        drive::setPinned(w, {QStringLiteral("R1"), QStringLiteral("R3")});
        drive::start(w);
        drive::setRace(w, true);
        for (int k = 0; k < 20; k++) {
            QVector<Pilot> ps{pilot(kR1), pilot(kR3)};
            if (k >= 2) ps.push_back(pilot(5748, 18));  // ajeno, encima de R3
            emit b.surveyUpdated(survey(k, ps));
        }
        drive::setRace(w, false);
        const QStringList s = drive::pilotSummary(w);
        for (const QString &l : s) std::printf("    %s\n", qUtf8Printable(l));
        check(s.size() >= 2 && s[0].startsWith(QStringLiteral("R3:")) && !s[0].startsWith(QStringLiteral("R3: 0 ")),
              "por piloto: R3 (intruso) el primero y con graves");
        check(s.size() >= 2 && s[1].startsWith(QStringLiteral("R1: 0 serious, 0 warnings")),
              "por piloto: R1 sin problemas");
        check(!s.isEmpty() && s.back().startsWith(QStringLiteral("race:")), "lo de toda la carrera, al final");
        const QString verdict = drive::find<QLabel>(&w, "historyVerdict")->text();
        check(verdict.startsWith(QStringLiteral("Most trouble: <b>Pilot 2 (R3)</b>: ")) &&
                  verdict.endsWith(QStringLiteral("No trouble: Pilot 1 (R1).")),
              "por piloto: el que mas problemas ha tenido (R3) y los que ninguno (R1)");
        check(s.filter(QStringLiteral("(race 1)")).size() == s.size(), "primera carrera: todo en la carrera 1");
        {
            // los eventos del elegido (R3, el primero): los graves abiertos, la informacion cerrada, sin eventos atenuado
            const QStringList e = drive::pilotEvents(w);
            check(e.size() == 7 && e[0].startsWith(QStringLiteral("Race 1 · Pilot 2 (R3): SNR ")) &&
                      e[1] == QStringLiteral("Serious (1) [+]") && e[2].startsWith(QStringLiteral("  → E8 ")) &&
                      e[2].contains(QStringLiteral("| +0:00 | Someone else is transmitting on top of it.")) &&
                      e[3] == QStringLiteral("Warnings (0) [-]") && e[4] == QStringLiteral("Channel changes (0) [-]") &&
                      e[5] == QStringLiteral("Information (1) [-]") &&
                      e[6].startsWith(QStringLiteral("  Summary (worst video) | +0:00 | SNR ")),
                  "eventos de R3: el intruso en graves (abierto), el resumen en informacion (cerrado)");
        }
        drive::setEventGroupOpen(w, 0, false);  // a mano: graves cerrado, informacion abierta
        drive::setEventGroupOpen(w, 3, true);
        drive::setRace(w, true);  // otra carrera, numerada aparte; en esta R3 no sale nunca (VTX apagado)
        for (int k = 20; k < 24; k++) emit b.surveyUpdated(survey(k, {pilot(kR1)}));
        drive::showHistory(w);  // la tabla en vivo (como se ve al abrir la pestaña)
        const QStringList s2 = drive::pilotSummary(w);
        for (const QString &l : s2) std::printf("    %s\n", qUtf8Printable(l));
        const QStringList r2 = s2.filter(QStringLiteral("(race 2)"));
        check(r2.size() == 3 && r2[0].startsWith(QStringLiteral("R3:")) && r2[1].startsWith(QStringLiteral("R1:")) &&
                  r2[2].startsWith(QStringLiteral("race:")),
              "segunda carrera: sus dos pilotos y lo general, aparte de la primera");
        check(s2.filter(QStringLiteral("(race 1)")).size() == s.size(), "la primera carrera sigue igual");
        {
            // rehecho en cada barrido de la segunda carrera: el mismo piloto conserva lo abierto y cerrado a mano
            const QStringList e = drive::pilotEvents(w);
            check(e.size() == 7 && e[1] == QStringLiteral("Serious (1) [-]") &&
                      e[5] == QStringLiteral("Information (1) [+]"),
                  "eventos de R3 rehechos: los grupos se quedan como los dejo el usuario");
            auto *pilots = drive::find<QTreeWidget>(&w, "historyPilots");
            pilots->setCurrentItem(pilots->topLevelItem(1));  // otro piloto: R3 en la segunda carrera
            check(drive::pilotEvents(w) ==
                      QStringList{QStringLiteral("Race 2 · Pilot 2 (R3): Its pilot was never seen."),
                                  QStringLiteral("Serious (0) [-]"), QStringLiteral("Warnings (0) [-]"),
                                  QStringLiteral("Channel changes (0) [-]"), QStringLiteral("Information (0) [-]")},
                  "otro piloto (R3, carrera 2, nunca visto): grupos vacios y cerrados");
            pilots->setCurrentItem(pilots->topLevelItem(0));  // y de vuelta a R3: como al principio
            const QStringList back = drive::pilotEvents(w);
            check(back.size() == 7 && back[1] == QStringLiteral("Serious (1) [+]") &&
                      back[5] == QStringLiteral("Information (1) [-]"),
                  "de vuelta a R3: sus grupos como al principio (graves abiertos)");
        }
    }

    {
        // mis pilotos (marcados en "En el aire"): cada uno, o ya esta en un canal recomendado (lo dice su fila) o
        // tiene que cambiar (lo dice el resumen), nunca las dos cosas ni ninguna. Se lee la ventana como el usuario
        FakeBackend b;
        ChannelAnalyzer w(&b);
        open(b);
        drive::setPinned(w, {});  // los fijados de los casos de antes se guardan en el .ini
        drive::setCounts(w, 3, 0);
        drive::start(w);
        const QVector<Pilot> air{pilot(kR1), pilot(5769), pilot(kR8)};
        for (int k = 0; k < 4; k++) emit b.surveyUpdated(survey(k, air));
        // la ventana les pone el nombre de su canal (src/channel_names.h): esa es la clave de "mio"
        const QStringList mine{QStringLiteral("R1"), QStringLiteral("R4"), QStringLiteral("R8")};
        drive::setMine(w, mine);
        QTreeWidget *table = nullptr;
        for (auto *t : w.findChildren<QTreeWidget *>())
            if (t->headerItem()->text(6) == QStringLiteral("Notes")) table = t;
        QString summary;
        for (auto *l : w.findChildren<QLabel *>())
            if (l->text().startsWith(QStringLiteral("Recommended:"))) summary = l->text();
        check(table && table->topLevelItemCount() == 3 && !summary.isEmpty(),
              "mios: la tabla con 3 recomendados y el resumen");
        int placed = 0, moving = 0, both = 0;
        for (const QString &ch : mine) {
            bool inRow = false;
            for (int i = 0; table && i < table->topLevelItemCount(); i++)
                inRow = inRow || table->topLevelItem(i)->text(6).contains(QStringLiteral("your pilot %1").arg(ch));
            const bool inSummary = summary.contains(QStringLiteral("<b>%1").arg(ch));
            placed += inRow && !inSummary;
            moving += inSummary && !inRow;
            both += inRow == inSummary;
        }
        std::printf("    mios: %d ya en un recomendado, %d tienen que cambiar\n", placed, moving);
        check(both == 0 && placed + moving == 3, "mios: cada uno, o en su fila o en 'tienen que cambiar'");
        // solo quedan R1, R4 y R8 por analizar: la recomendacion son sus canales y ninguno tiene que cambiar
        QStringList others;
        for (const char *band : {"A", "B", "E", "F", "R"})
            for (int n = 1; n <= 8; n++) others << QStringLiteral("%1%2").arg(QLatin1String(band)).arg(n);
        for (const QString &ch : mine) others.removeAll(ch);
        drive::setExcluded(w, others);
        for (int k = 4; k < 8; k++) emit b.surveyUpdated(survey(k, air));
        summary.clear();
        for (auto *l : w.findChildren<QLabel *>())
            if (l->text().startsWith(QStringLiteral("Recommended:"))) summary = l->text();
        int here = 0;
        for (int i = 0; table && i < table->topLevelItemCount(); i++)
            here += table->topLevelItem(i)->text(6).contains(QStringLiteral("is already here"));
        check(here == 3 && !summary.contains(QStringLiteral("must change")),
              "mios: los tres ya en los recomendados (R1, R4, R8): ninguno tiene que cambiar");
    }
    {
        // cursor: con el raton sobre la grafica, una linea en esa frecuencia; al salir, nada
        FakeBackend b;
        ChannelAnalyzer w(&b);
        SpectrumView *v = drive::view(w);
        v->resize(760, 300);
        const drive::Hover h = drive::hover(v, 380);
        check(h.marksColumn(380), "cursor: con el raton encima se marca su frecuencia");
        check(h.after == h.before, "cursor: al salir el raton no queda nada");
        const drive::Hover margin = drive::hover(v, 3);
        check(margin.over == margin.before, "cursor: fuera del eje de frecuencias no se marca nada");
        // lo que dice: MHz y canales cerca (como los pilotos, src/channel_names.h); los dB del MHz medido ahi
        const QVector<SpectrumPoint> cells{{5804.5, 3.f, 6.f, 0.f}, {5805.5, 12.4f, 20.f, 0.f}};
        const QStringList t = SpectrumView::cursorText(5805.7, cells);
        std::printf("    cursor en 5805.7: %s\n", qPrintable(t.join(QStringLiteral(" | "))));
        check(t.size() == 2 && t[0].startsWith(QStringLiteral("5806 MHz · R5/")) &&
                  t[1] == QStringLiteral("avg 12 dB · peak 20 dB"),
              "cursor: MHz, canales cerca y dB medios y de pico");
        check(SpectrumView::cursorText(5990, cells) == QStringList{QStringLiteral("5990 MHz")},
              "cursor: sin canal cerca ni medida, solo los MHz");
    }
    {
        // leyenda plegable: se recuerda al volver a abrir la ventana
        FakeBackend b;
        ChannelAnalyzer w(&b);
        check(drive::legendShown(w), "leyenda: se ve la primera vez");
        drive::toggleLegend(w);
        check(!drive::legendShown(w), "leyenda: su boton la pliega");
        ChannelAnalyzer w2(&b);
        check(!drive::legendShown(w2), "leyenda: plegada al volver a abrir");
        drive::toggleLegend(w2);
        check(drive::legendShown(w2), "leyenda: su boton la despliega");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
