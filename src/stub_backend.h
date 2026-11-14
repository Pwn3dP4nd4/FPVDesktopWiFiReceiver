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
    void openDevice() override;
    void closeDevice() override;
    void startScan() override;
    void watch(double freqMHz) override;
    void stop() override;

private:
    void tick();
    void ensureTimer();
    QTimer *timer_ = nullptr;
    int n_ = 0;
    double watching_ = 0;
};
