#pragma once
#include "backend.h"

#include <atomic>
#include <memory>
#include <vector>

class QTimer;
namespace kp {
class Radio;
struct Frame;
}

// Backend real sobre kp::Radio (third_party/kepyur/native/radio): lo mismo que hace native/jni en Android,
// pero los fotogramas salen como QImage en lugar de pintarse en una Surface.
class RadioBackend : public Backend {
    Q_OBJECT
public:
    explicit RadioBackend(QObject *parent = nullptr);
    ~RadioBackend() override;
    QString name() const override { return QStringLiteral("radio"); }

public slots:
    void openDevice() override;
    void closeDevice() override;
    void startScan() override;
    void setScanChannels(const QVector<double> &mhz) override;
    void watch(double freqMHz) override;
    void stop() override;
    void setWatchRate(int rateSel) override;
    void setGain(double db) override;
    void record(double seconds, const QString &prefix) override;

private:
    void ensureObjects();
    QString openRadio();  // cambio a USB 3 + apertura; "" si fue bien, si no el error con la ayuda
    void poll();
    void recover(const QString &mode, double watchMHz);
    void adapterLost(const QString &mode, double watchMHz);
    void tryReconnect();
    void onFrame(const kp::Frame &f);  // hilo decodificador de kp::Radio
    std::unique_ptr<kp::Radio> radio_;
    QTimer *poll_ = nullptr;
    QTimer *reconnect_ = nullptr;  // tras desenchufar: comprobar cada 2 s si el adaptador ha vuelto
    QString resumeMode_;           // lo que se hacia al perderlo, para retomarlo
    double resumeMHz_ = 0, lastWatchMHz_ = 0;
    QByteArray lastPilotsKey_;  // lo que se muestra de los pilotos (sin la edad)
    bool forcePilots_ = false;
    unsigned polls_ = 0;
    qint64 lastRecoverMs_ = 0;
    std::vector<double> scanPlan_;  // centros de paso (Hz); vacio = plan completo de kepyur
};
