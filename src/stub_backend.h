#pragma once
#include "backend.h"

class QTimer;

// Backend de demostracion: sin hardware. Barras de color y dos pilotos falsos (opcion --demo).
class StubBackend : public Backend {
    Q_OBJECT
public:
    explicit StubBackend(QObject *parent = nullptr);
    QString name() const override { return QStringLiteral("demo"); }

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
    int sweeps_ = 0;
    void surveyTick();
};
