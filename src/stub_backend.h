#pragma once
#include "backend.h"

class QTimer;

// Backend de demostracion: sin hardware. Barras de color y dos pilotos falsos (opcion --demo).
class StubBackend : public Backend {
    Q_OBJECT
public:
    explicit StubBackend(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("demo"); }
    // Analizador: escenario de carrera (test/analyzer_shot.cpp --race): cuatro analogicos en R1, R3, R6 y R8 y, desde
    // el barrido 5, un VTX ajeno encima de R3; un barrido cada 200 ms. Antes de startSurvey.
    void setRaceDemo(bool on) { race_ = on; }

public slots:
    void setDigitalScan(bool on) override { digital_ = on; }
    void openDevice() override;
    void closeDevice() override;
    void startScan() override;
    void setScanChannels(const QVector<double> &mhz) override { scanMHz_ = mhz; }
    void watch(double freqMHz) override;
    void stop() override;
    void startSurvey(double loMHz, double hiMHz) override;
    void stopSurvey() override;
    void setSurveyMemory(int sweeps) override { memory_ = sweeps; }

private:
    void tick();
    void ensureTimer();
    QTimer *timer_ = nullptr;
    int n_ = 0;
    double watching_ = 0;
    QVector<double> scanMHz_;  // vacio = todos
    bool digital_ = false;     // modo experimental: tambien el piloto HDZero simulado
    QTimer *survey_ = nullptr;  // analizador simulado: un barrido por segundo
    double surveyLo_ = 0, surveyHi_ = 0;
    int memory_ = 20;  // con menos barridos, las rafagas simuladas se ven antes
    int sweeps_ = 0;
    bool race_ = false;
    void surveyTick();
    void raceTick(SurveyData &d);
};
