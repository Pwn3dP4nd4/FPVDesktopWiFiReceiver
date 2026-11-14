#pragma once
#include "backend.h"

#include <atomic>
#include <memory>

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
    void watch(double freqMHz) override;
    void stop() override;
    void setWatchRate(int rateSel) override;
    void setGain(double db) override;
    void record(double seconds, const QString &prefix) override;

private:
    void poll();
    void recover(const QString &mode, double watchMHz);
    void onFrame(const kp::Frame &f);  // hilo decodificador de kp::Radio
    std::unique_ptr<kp::Radio> radio_;
    QTimer *poll_ = nullptr;
    QByteArray lastPilotsJson_;
    unsigned polls_ = 0;
    qint64 lastRecoverMs_ = 0;
};
