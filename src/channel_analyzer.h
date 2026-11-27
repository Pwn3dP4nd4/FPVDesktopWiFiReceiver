#pragma once
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <algorithm>
#include <QFontMetrics>
#include <QVector>

#include <optional>

#include "backend.h"
#include "channel_advisor.h"
#include "race_history.h"
#include "race_monitor.h"
#include "survey_session.h"

class QCheckBox;
class QFont;
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

// Grafica del analizador: espectro medio y picos (dB sobre el suelo), zonas con rafagas, los canales candidatos
// coloreados por interferencia, lo que ya hay en el aire y los canales recomendados (los fijados, distintos).
// Arriba, la barra de barrido: que parte del espectro se acaba de medir (SpectrumPoint::seq), brillante al refrescarse
// y apagandose hasta la siguiente pasada; donde va ahora el barrido, una franja tenue sobre la grafica.
// Clic derecho sobre una marca de canal: fijarlo o desfijarlo (pinToggled). Con el raton encima, su frecuencia.
class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent = nullptr);
    void setData(const SurveyData &d, const advisor::Advice &a, double loMHz, double hiMHz,
                 const QStringList &mine = {});
    QSize sizeHint() const override { return {760, 300}; }
    // analisis en marcha: la barra de barrido se anima; al empezar, lo de antes se olvida
    void setScanning(bool on);
    // test/analyzer_logic_test.cpp (solo se ve en los pixeles de la barra): MHz (de 1 en 1) refrescados en la ultima
    // actualizacion y los ya medidos alguna vez
    QVector<int> sweepHeadForTest() const { return head_; }
    int sweepCellsForTest() const { return int(cellT_.size()); }

signals:
    void pinToggled(const QString &name, bool hdzero);  // fijar si no lo estaba, desfijar si lo estaba
    void unpinAllRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    bool isPinned(const advisor::Candidate &c) const;
    // ---- paintEvent(), por capas ----
    struct Geometry {  // ejes del pintado: x por MHz (lo..hi), y por dB (0..dbMax); filas de canales debajo
        QFontMetrics fm;
        int rowH = 0, left = 0, right = 0, barTop = 0, barH = 0, top = 0, plotBottom = 0;
        double lo = 0, hi = 1, dbMax = 40;
        double x(double mhz) const { return left + (mhz - lo) / (hi - lo) * (right - left); }
        double y(double db) const { return plotBottom - std::clamp(db, 0.0, dbMax) / dbMax * (plotBottom - top); }
        double mhz(double px) const { return lo + (px - left) / (right - left) * (hi - lo); }
    };
    void drawAxes(QPainter &p, const Geometry &g) const;
    void drawPicks(QPainter &p, const Geometry &g) const;      // los recomendados (franjas) y los fijados
    void drawSpectrum(QPainter &p, const Geometry &g) const;   // rafagas, picos y media
    void drawWifi(QPainter &p, const Geometry &g) const;       // las redes WiFi que recuerda el analisis
    void drawSweepBar(QPainter &p, const Geometry &g) const;   // la barra de barrido
    void drawSweepHead(QPainter &p, const Geometry &g, qint64 t) const;  // donde va ahora
    void drawAir(QPainter &p, const Geometry &g) const;        // lo que hay en el aire
    void drawChannelRows(QPainter &p, const Geometry &g) const;
    void drawCursor(QPainter &p, const Geometry &g) const;     // la frecuencia bajo el raton
    SurveyData data_;              // los bins juntados de 1 en 1 MHz (se dibujan mas limpios)
    advisor::Advice advice_;
    QStringList mine_;
    double lo_ = 5640, hi_ = 5950;
    // geometria del ultimo pintado (menu contextual): eje x y filas de canales
    double left_ = 0, right_ = 1, rowsTop_ = 0, rowH_ = 0, plotBottom_ = 0;
    std::optional<double> mouseX_;  // el raton, si esta sobre la grafica

    // ---- barra de barrido: por MHz (floor), el ultimo seq visto y cuando cambio (ms de sweepClock_) ----
    void trackSweep();  // tras juntar data_.bins: que celdas se han medido desde la ultima vez
    QHash<int, quint32> cellSeq_;
    QHash<int, qint64> cellT_;
    QVector<int> head_;         // las de la ultima actualizacion con algo nuevo (donde va el barrido)
    qint64 headT_ = -1;
    double periodS_ = 3;        // lo que tarda el barrido en volver a pasar por el mismo sitio (media)
    QElapsedTimer sweepClock_;
    QTimer *anim_ = nullptr;    // repinta mientras se barre (la barra se va apagando)
    bool scanning_ = false;
};

// Analizador de canales (Herramientas): barre las bandas elegidas sin parar y recomienda canales para N pilotos
// analogicos y M HDZero (src/channel_advisor.h). No se ve video mientras esta en marcha.
class ChannelAnalyzer : public QDialog {
    Q_OBJECT
public:
    ChannelAnalyzer(Backend *backend, QWidget *parent = nullptr);
    void setAdapterOpen(bool open);
    bool isRunning() const { return session_.running(); }
    // el analizador de 2,4 GHz se ha puesto en marcha (el barrido ya es suyo): parar sin pedirselo al backend
    void abandon(const QString &why);
    // Las pruebas manejan la ventana por sus controles (test/analyzer_driver.h). Esto no se ve en ellos: el reloj del
    // analisis (s; < 0: el real) y cuantas veces se ha recalculado la recomendacion (una por barrido, no por
    // actualizacion: test/analyzer_logic_test.cpp)
    void setTimeForTest(double s) { session_.setTimeForTest(s); }
    int recomputesForTest() const { return recomputes_; }

signals:
    void requestStart(double loMHz, double hiMHz);
    void requestMemory(int sweeps);
    void requestStop();
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    // ---- el constructor, por bloques de la ventana ----
    QWidget *buildSide();
    void addBandsPanel(QVBoxLayout *side);
    void addPilotsPanel(QVBoxLayout *side);
    void narrowChanged(bool on);
    void regionChanged();
    QWidget *buildLegend();
    QWidget *buildAdvicePage();
    QWidget *buildRacePage();
    QHBoxLayout *buildArmRow();
    void raceTableMenu(const QPoint &pos);
    void retranslate();
    void toggle();
    void stop();
    void onStopped();     // el backend lo ha parado (otra orden): solo la ventana
    void showStopped(const QString &state, const QString &why);  // ya parado: boton, estado, carrera
    QByteArray airKey() const;        // los pilotos en el aire, sin la edad (cambia: hay que recomendar de nuevo)
    QStringList mineForView() const;  // mios para la grafica: marcados y en un canal fijado
    void updateAir();     // lista de pilotos en el aire
    void onSurvey(const SurveyData &d);
    void recompute();
    // ---- recompute(), por partes ----
    advisor::Advice pinnedOnlyAdvice() const;  // sin barridos: solo los fijados, sin medir
    QTreeWidgetItem *addPickRow(const advisor::Pick &p);  // fila de la tabla: casilla, sistema, canal, MHz
    void showUnmeasured(const advisor::Advice &a);  // tabla y resumen sin barridos
    // fila de un canal recomendado; mineHere: mis pilotos que ya estan en el (race::already_on)
    void addAdviceRow(const advisor::Pick &p, const std::vector<advisor::Wifi> &wifi, const QStringList &mineHere);
    QString summaryText(const advisor::Advice &a, const QStringList &toMove) const;
    static QStringList wifiChannels(const std::vector<advisor::Wifi> &wifi, const QString &dash);  // "1", "5-7"
    void logAdvice(const advisor::Advice &a, const QStringList &moving);  // al log, solo si cambia
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
    void refreshMoves();  // moves_: las recomendaciones de mover de ahora (las mostradas se mantienen si siguen valiendo)
    // fila i de la tabla de la carrera; worst: resaltada (el peor video). false: su recomendacion no es OK
    bool addRaceRow(int i, bool worst, const QFont &bold);
    QString raceAdviceText(bool allOk) const;  // bajo la tabla: los cambios a hacer, o que no hace falta ninguno
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

    // ---- historial de la carrera (src/race_history.h): avisos, recomendaciones, cambios y el resumen final ----
    using Ev = RaceHistory::Ev;
    void addEvent(Ev kind, const QString &channel, const QString &what, const QString &detail = QString(),
                  int pilot = -1);  // con el barrido de race_
    void addSummary();      // al parar: como ha ido cada canal
    void beginRaceRec();    // empieza el seguimiento (o vuelve a empezar con otros fijados)
    void updateHistoryTab(int events);  // el titulo de la pestana
    RaceHistory *history_ = nullptr;

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
    SurveySession session_;        // en marcha, adaptador, paradas pendientes y el reloj del analisis
    double surveyT_ = 0;           // s de session_.now() en el ultimo barrido recibido
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
    // por barrido, no por actualizacion: el backend manda el espectro cada segundo y un barrido tarda ~3 s. El
    // seguimiento de carrera, la memoria WiFi y la recomendacion solo avanzan con un barrido nuevo (o, la
    // recomendacion, si cambian los pilotos en el aire); en medio solo se repinta la grafica
    int lastSweep_ = -1;
    QByteArray airKey_;           // los pilotos en el aire (sin la edad) del ultimo recompute
    advisor::Advice advice_;      // la ultima recomendacion (para repintar la grafica entre barridos)
    int recomputes_ = 0;
};
