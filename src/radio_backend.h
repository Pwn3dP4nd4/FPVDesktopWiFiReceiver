#pragma once
#include "backend.h"

#include <atomic>
#include <memory>
#include <vector>

class QTimer;
class QJsonArray;
namespace kp {
struct Frame;
}
namespace fpv {
class Radio;
}

// Backend real sobre fpv::Radio (src/radio, derivada de native/radio de kepyur): lo mismo que hace native/jni en Android,
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
    void setDigitalScan(bool on) override;
    void startSurvey(double loMHz, double hiMHz) override;
    void stopSurvey() override;
    void setSurveyMemory(int sweeps) override;

private:
    void ensureObjects();
    QString openRadio();  // cambio a USB 3 + apertura; "" si fue bien, si no el error con la ayuda
    void poll();
    void recover(const QString &mode, double watchMHz);
    // Cierra y espera a que vuelva (tryReconnect); why: el texto para la GUI (vacio = desenchufado)
    void adapterLost(const QString &mode, double watchMHz, const QString &why = {});
    void tryReconnect();
    void onFrame(const kp::Frame &f);  // hilo decodificador de fpv::Radio
    std::unique_ptr<fpv::Radio> radio_;
    QTimer *poll_ = nullptr;
    QTimer *reconnect_ = nullptr;  // tras desenchufar: comprobar cada 2 s si el adaptador ha vuelto
    QString resumeMode_;           // lo que se hacia al perderlo, para retomarlo
    double resumeMHz_ = 0, lastWatchMHz_ = 0;
    QByteArray lastPilotsKey_;  // lo que se muestra de los pilotos (sin la edad)
    bool forcePilots_ = false;
    unsigned polls_ = 0;
    qint64 lastRecoverMs_ = 0;
    QString lastFault_;  // ultimo last_fault registrado (cada fallo del flujo, una linea en el log)
    std::vector<double> scanPlan_;  // centros de paso (Hz); vacio = plan completo
    bool digitalScan_ = false;      // modo experimental (se aplica tambien a una radio creada despues)
    int surveyMemory_ = 20;            // analizador: barridos en la media (setSurveyMemory)
    std::vector<double> surveyPlan_;  // analizador de canales en marcha: sus pasos; vacio = no
    // analizador de 2,4 GHz (ELRS): la banda (Hz) y solo espectro (sin buscar video ni senales digitales)
    double surveyLoHz_ = 0, surveyHiHz_ = 0;
    bool surveyOnly_ = false;
    void radioSurvey();  // fpv::Radio::survey() con la banda del analizador en marcha
    void emitSurvey(const QJsonArray &pilots);
};
