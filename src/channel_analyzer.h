#pragma once
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
    // marcar como mios en "En el aire" (clave: el nombre del piloto o sus MHz); se aplica a lo que ya se ve
    void setMineForTest(const QStringList &keys);

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
    void updateAir();     // lista de pilotos en el aire
    void onSurvey(const SurveyData &d);
    void recompute();
    QString bands() const;
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
    // cambio a mano (tabla, grafica, quitar todos): guarda, reinicia el seguimiento si esta activo y recalcula
    void setPins(const QVector<Pin> &pins);
    void togglePin(const QString &name, bool hdzero);
    void savePins() const;
    bool pinnedMine(const Pilot &p) const;  // un piloto en un canal fijado cuenta como mio (no hace falta marcarlo)
    void setRace(bool on);
    void updateRaceControls();
    void updateRace();  // panel de carrera
    void applyMoves(const std::vector<race::Move> &moves);  // fija los canales nuevos (Aplicar)
    bool canApplyAlone(const race::Move &m) const;  // compatible con los fijados de ahora (sin las demas)

    QVector<Pin> pinned_;
    race::Monitor race_;
    bool raceOn_ = false;
    std::vector<race::Move> moves_;  // las recomendaciones mostradas (columna Recomendacion, boton Apply)
    QTabWidget *tabs_ = nullptr;
    QLabel *pinHint_ = nullptr;
    QPushButton *unpinBtn_ = nullptr;
    QCheckBox *raceBox_ = nullptr;
    QLabel *raceState_ = nullptr, *raceAdvice_ = nullptr;
    QPushButton *applyBtn_ = nullptr;
    QTreeWidget *raceTable_ = nullptr;

    QVector<QCheckBox *> bandBoxes_;
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
};
