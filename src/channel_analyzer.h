#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QVector>

#include "backend.h"
#include "channel_advisor.h"

class QCheckBox;
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;

// Grafica del analizador: espectro medio y picos (dB sobre el suelo), zonas con rafagas, los canales candidatos
// coloreados por interferencia, lo que ya hay en el aire y los canales recomendados.
class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent = nullptr);
    void setData(const SurveyData &d, const advisor::Advice &a, double loMHz, double hiMHz,
                 const QStringList &mine = {});
    QSize sizeHint() const override { return {760, 300}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    SurveyData data_;              // los bins juntados de 1 en 1 MHz (se dibujan mas limpios)
    advisor::Advice advice_;
    QStringList mine_;
    double lo_ = 5640, hi_ = 5950;
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
