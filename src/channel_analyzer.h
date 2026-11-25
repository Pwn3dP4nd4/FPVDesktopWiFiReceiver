#pragma once
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QVector>

#include <optional>

#include "backend.h"
#include "channel_advisor.h"
#include "race_monitor.h"

class QCheckBox;
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QToolButton;
class QTreeWidget;

// Grafica del analizador: espectro medio y picos (dB sobre el suelo), zonas con rafagas, los canales candidatos
// coloreados por interferencia, lo que ya hay en el aire y los canales recomendados (los fijados, distintos).
// Clic derecho sobre una marca de canal: fijarlo o desfijarlo (pinToggled).
class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent = nullptr);
    void setData(const SurveyData &d, const advisor::Advice &a, double loMHz, double hiMHz,
                 const QStringList &mine = {});
    QSize sizeHint() const override { return {760, 300}; }

signals:
    void pinToggled(const QString &name, bool hdzero);  // fijar si no lo estaba, desfijar si lo estaba
    void unpinAllRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    bool isPinned(const advisor::Candidate &c) const;
    SurveyData data_;              // los bins juntados de 1 en 1 MHz (se dibujan mas limpios)
    advisor::Advice advice_;
    QStringList mine_;
    double lo_ = 5640, hi_ = 5950;
    // geometria del ultimo pintado (menu contextual): eje x y filas de canales
    double left_ = 0, right_ = 1, rowsTop_ = 0, rowH_ = 0, plotBottom_ = 0;
};

// Analizador de canales (Herramientas): barre las bandas elegidas sin parar y recomienda canales para N pilotos
// analogicos y M HDZero (src/channel_advisor.h). No se ve video mientras esta en marcha.
class ChannelAnalyzer : public QDialog {
    Q_OBJECT
public:
    ChannelAnalyzer(Backend *backend, QWidget *parent = nullptr);
    void setAdapterOpen(bool open);
    // test/analyzer_shot.cpp: pilotos (< 0 = los guardados) y empezar sin clic
    void setCounts(int analog, int hdzero);
    void startForTest() { if (!running_) toggle(); }
    // canales fijados ("R3", "hdz:R3"; se guardan como los del usuario) y activar el seguimiento de carrera
    void setPinnedForTest(const QStringList &pins);
    void setRaceForTest(bool on);
    void applyForTest() { applyMoves(moves_); }  // pulsar Aplicar (todas las recomendaciones)
    void setExcludedForTest(const QStringList &names);  // canales sin analizar ("R2")
    void openChannelMenuForTest(int band) { showChannelMenu(band, bandBtns_[band]); }
    // marcar como mios en "En el aire" (clave: el nombre del piloto o sus MHz); se aplica a lo que ya se ve
    void setMineForTest(const QStringList &keys);
    // pulsar "Empezar al salir los pilotos" con este numero de pilotos (<= 0: todos los fijados)
    void armForTest(int pilots);
    void showHistoryForTest();  // pestana Historial
    // test/analyzer_logic_test.cpp: reloj del analisis (s) en lugar del real (< 0: el real) y estado interno
    void setTimeForTest(double s) { testTimeS_ = s; }
    bool runningForTest() const { return running_; }
    bool raceOnForTest() const { return raceOn_; }
    bool armedForTest() const { return armed_; }
    int raceSweepsForTest() const { return race_.sweeps(); }
    int recomputesForTest() const { return recomputes_; }
    QStringList pinnedForTest() const;   // "R3", "hdz:R3", en el orden de pinned_
    QStringList historyForTest() const;  // lo que dice cada evento del Historial (RaceEvent::what)

signals:
    void requestStart(double loMHz, double hiMHz);
    void requestMemory(int sweeps);
    void requestStop();
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void toggle();
    void stop();
    void onStopped();     // el backend lo ha parado (otra orden): solo la ventana
    double now() const;   // s desde que empezo el analisis (clock_, o el de setTimeForTest)
    QByteArray airKey() const;        // los pilotos en el aire, sin la edad (cambia: hay que recomendar de nuevo)
    QStringList mineForView() const;  // mios para la grafica: marcados y en un canal fijado
    void updateAir();     // lista de pilotos en el aire
    void onSurvey(const SurveyData &d);
    void recompute();
    QString bands() const;
    // Candidatos de las bandas marcadas sin los canales quitados (excluded_): lo que se analiza y se recomienda
    std::vector<advisor::Candidate> analogCands() const;
    std::vector<advisor::Candidate> hdzCands() const;
    bool haveChannels() const;  // queda algun canal que analizar
    // canales de una banda: desplegable con una casilla por canal
    void showChannelMenu(int band, QToolButton *anchor);
    void setExcluded(const QStringList &names, bool excluded);  // y vuelve a barrer si cambia la zona
    void updateBandTexts();  // "R  Raceband (6/8)"
    void bandsChanged();     // guarda, rehace la zona a barrer y recalcula
    void range(double &lo, double &hi) const;
    QString noteText(const advisor::Pick &p, const std::vector<advisor::Wifi> &wifi) const;
    // los bins para el recomendador (mis pilotos quitados) y los pilotos ajenos; all: tambien los mios
    void prepare(std::vector<advisor::Bin> &bins, std::vector<advisor::Signal> &seen,
                 std::vector<advisor::Signal> *all = nullptr);
    advisor::Region region() const;
    void feedWifi();  // last_ -> wifiMem_

    // ---- canales fijados (ya repartidos a los pilotos: se guardan en "analyzer/pinned") y carrera ----
    struct Pin {
        QString name;  // "R3"
        bool hdzero = false;
    };
    QVector<Pin> parsePins(const QStringList &s) const;  // "R3", "hdz:R3"; los que no existen se ignoran
    std::vector<advisor::Candidate> pinnedCandidates() const;  // en el orden de pinned_ (= race_.health())
    bool isPinned(const QString &name, bool hdzero) const;
    QString pinsText() const;  // "R1, R3 HDZero"
    // cambio a mano (tabla, grafica, quitar todos): guarda, reinicia el seguimiento si esta activo y recalcula
    void setPins(const QVector<Pin> &pins);
    void togglePin(const QString &name, bool hdzero);
    void savePins() const;
    bool pinnedMine(const Pilot &p) const;  // un piloto en un canal fijado cuenta como mio (no hace falta marcarlo)
    // why: por que empieza o para (para el historial); vacio = a mano con la casilla
    void setRace(bool on, const QString &why = QString());
    void updateRaceControls();
    void updateRace();  // panel de carrera
    void applyMoves(const std::vector<race::Move> &moves);  // fija los canales nuevos (Aplicar)
    // las de moves que siguen entre las recomendaciones de ahora (mismo piloto y canal nuevo) con la carrera en marcha:
    // un menu abierto mientras llegan barridos no aplica una ya pasada
    std::vector<race::Move> stillValid(const std::vector<race::Move> &moves) const;
    // texto de la columna Recomendacion de un canal en carrera (y su explicacion y color); noBetter: dice "sin canal
    // mejor" (noBetter_ para el siguiente barrido)
    void rowAdvice(int i, QString &rec, QString &tip, QColor &col, bool &noBetter) const;
    QVector<bool> noBetter_;  // por fila de la carrera: la ultima vez decia "sin canal mejor" (margen para quitarlo)
    void logRaceRow(int i, const QString &rec, const QString &tip, const QColor &col);  // al log y al Historial
    QString raceStateText() const;
    QString healthText(const race::ChannelHealth &h, bool summary) const;  // SNR, calidad, cortes...
    bool canApplyAlone(const race::Move &m) const;  // compatible con los fijados de ahora (sin las demas)
    // lo que esta en el aire para el seguimiento (visto hace menos de kRaceHoldS), con su SNR
    std::vector<advisor::Signal> raceSignals() const;

    // ---- auto-inicio: armado, espera a que salgan armCount_ pilotos en sus canales fijados y empieza solo ----
    void setArmed(bool on);
    void checkArmed(const std::vector<advisor::Signal> &now);
    void checkLanded(const std::vector<advisor::Signal> &now);  // fin automatico: ya no queda ningun piloto
    QVector<double> pinLastT_;    // por fijado (como pinned_): surveyT_ de la ultima vez que se le vio en carrera
    bool lingerWarned_ = false;   // ya se ha avisado de que una senal sola impide el fin automatico
    bool armed_ = false;
    int armPresent_ = 0;          // pilotos en su canal en el ultimo barrido (mientras esta armado)
    double lastPresentT_ = 0;     // surveyT_ de la ultima vez que se vio a alguno en carrera
    bool raceSeenAny_ = false;    // en esta carrera se ha visto ya a alguno (si no, no se para sola)
    QPushButton *armBtn_ = nullptr;
    QSpinBox *armCount_ = nullptr;  // 1..fijados; el maximo se muestra "todos"
    bool armAll_ = true;            // "todos": sigue en el maximo aunque cambie el numero de fijados
    int armWant() const;            // pilotos que hacen falta para empezar
    QLabel *armLbl_ = nullptr;
    QCheckBox *autoStop_ = nullptr;

    // ---- historial de la carrera: avisos, recomendaciones, cambios aplicados y el resumen final ----
    enum class Ev { Info, Ok, Warn, Move, Applied, Summary };
    struct RaceEvent {
        QDateTime when;
        double raceS = 0;  // s desde que empezo el seguimiento
        int sweep = 0;
        Ev kind = Ev::Info;
        QString channel, what, detail;
    };
    void addEvent(Ev kind, const QString &channel, const QString &what, const QString &detail = QString());
    void addSummary();  // al parar: como ha ido cada canal
    QString historyText(bool csv) const;
    void saveHistory();
    void updateHistoryTab();
    QVector<RaceEvent> events_;   // de todas las carreras de esta ventana (Borrar las quita)
    QElapsedTimer raceClock_;
    QTreeWidget *histTable_ = nullptr;
    QLabel *histHint_ = nullptr;
    QPushButton *histCopy_ = nullptr, *histSave_ = nullptr, *histClear_ = nullptr;

    QVector<Pin> pinned_;
    race::Monitor race_;
    bool raceOn_ = false;
    std::vector<race::Move> moves_;  // las recomendaciones mostradas (columna Recomendacion, boton Apply)
    // lo ultimo escrito en el log (recomendacion y estado de cada canal en carrera): solo se escribe al cambiar
    QString adviceLogKey_;
    QStringList raceLogKeys_;
    QTabWidget *tabs_ = nullptr;
    QLabel *pinHint_ = nullptr;
    QPushButton *unpinBtn_ = nullptr;
    QCheckBox *raceBox_ = nullptr;
    QLabel *raceState_ = nullptr, *raceAdvice_ = nullptr;
    QPushButton *applyBtn_ = nullptr;
    QTreeWidget *raceTable_ = nullptr;

    QVector<QCheckBox *> bandBoxes_;
    QVector<QToolButton *> bandBtns_;
    QStringList excluded_;  // canales de las bandas marcadas que no se analizan ("R2"; "analyzer/excluded")
    QSpinBox *nAnalog_ = nullptr, *nHdz_ = nullptr;
    QCheckBox *narrow_ = nullptr;
    QComboBox *memory_ = nullptr;
    QComboBox *region_ = nullptr;  // reglas WiFi (UE / EE. UU.): hasta donde llega el bloque de cada canal
    QLabel *regionLbl_ = nullptr;
    advisor::WifiMemory wifiMem_;  // redes WiFi vistas en este analisis (un AP callado se ve a ratos)
    QElapsedTimer clock_;          // desde que empezo el analisis
    double surveyT_ = 0;           // s de clock_ en el ultimo barrido recibido
    QLabel *memoryLbl_ = nullptr;
    QListWidget *air_ = nullptr;  // pilotos en el aire; marcados = mios (su canal no cuenta como ocupado)
    QLabel *airTitle_ = nullptr;
    QStringList mine_;            // nombres de canal (o MHz) marcados como mios
    QVector<int> seenIdx_;        // Signal pasada al recomendador -> indice en last_.pilots
    QPushButton *startBtn_ = nullptr;
    QLabel *bandsTitle_ = nullptr, *pilotsTitle_ = nullptr, *analogLbl_ = nullptr, *hdzLbl_ = nullptr;
    QLabel *hint_ = nullptr, *state_ = nullptr, *summary_ = nullptr;
    QVector<QLabel *> legendText_;
    SpectrumView *view_ = nullptr;
    QTreeWidget *table_ = nullptr;
    SurveyData last_;
    bool running_ = false, open_ = false;
    // por barrido, no por actualizacion: el backend manda el espectro cada segundo y un barrido tarda ~3 s. El
    // seguimiento de carrera, la memoria WiFi y la recomendacion solo avanzan con un barrido nuevo (o, la
    // recomendacion, si cambian los pilotos en el aire); en medio solo se repinta la grafica
    int lastSweep_ = -1;
    QByteArray airKey_;           // los pilotos en el aire (sin la edad) del ultimo recompute
    advisor::Advice advice_;      // la ultima recomendacion (para repintar la grafica entre barridos)
    int recomputes_ = 0;
    int pendingStops_ = 0;        // requestStop emitidos cuyo surveyStopped aun no ha llegado
    double testTimeS_ = -1;
};
