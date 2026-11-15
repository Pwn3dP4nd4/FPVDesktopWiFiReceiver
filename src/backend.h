#pragma once
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <mutex>
struct Pilot {
    double freqMHz = 0;
    double snrDb = 0;
    QString name;   // canal estandar ("R4"), vacio si no coincide
    QString stdName;  // NTSC / PAL
    int ageS = 0;   // segundos desde el ultimo barrido que lo vio
    QString label() const {
        QString s = name.isEmpty() ? QStringLiteral("%1 MHz").arg(freqMHz, 0, 'f', 0)
                                   : QStringLiteral("%1  %2 MHz").arg(name).arg(freqMHz, 0, 'f', 0);
        if (!stdName.isEmpty()) s += QStringLiteral("  ") + stdName;
        return s;
    }
};

// Resumen de kp::Radio::status_json() para la GUI.
struct RadioStatus {
    bool open = false;
    bool usb3 = false;
    QString chip;
    QString mode;      // idle / scan / watch / test
    QString rxState, rxError;
    double watchMHz = 0;
    double msps = 0;
    bool fallback = false;
    double snrDb = -1;
    int scanStep = 0, scanSteps = 0;
    bool locked = false;
    double lock = 0;
    QString videoStd;
    double fps = 0;
    bool colour = false;
    quint64 dropped = 0, restarts = 0;
};

Q_DECLARE_METATYPE(Pilot)
Q_DECLARE_METATYPE(RadioStatus)

// Interfaz entre la GUI y la radio. Vive en un QThread propio: los slots bloqueantes (openDevice tarda ~5 s
// en arrancar el chip) no congelan la ventana. Los signals llegan a la GUI en cola (QueuedConnection).
class Backend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~Backend() override = default;
    virtual QString name() const = 0;

    // Control de flujo de video (thread-safe, llamada directa, no slot): la GUI avisa al pintar cada frame;
    // el backend descarta frames mientras haya demasiados en cola (el ultimo gana, el DSP nunca espera).
    void frameConsumed() { if (inFlight_.fetch_sub(1) <= 0) inFlight_.store(0); }
    static inline std::atomic<int> gatedFrames{0};  // descartados por ir la GUI retrasada (medidas)

    // Derivacion de TODOS los fotogramas, antes del control de flujo de la GUI (grabacion de video: aunque la
    // ventana vaya retrasada no se pierde ninguno). Se llama en el hilo del decodificador; thread-safe. Al
    // volver de setFrameTap({}) ya no hay ninguna llamada en curso al anterior.
    using FrameTap = std::function<void(const QImage &frame, qint64 producedNs)>;
    void setFrameTap(FrameTap tap) {
        std::lock_guard<std::mutex> l(tapMu_);
        tap_ = std::move(tap);
        tapOn_.store(bool(tap_), std::memory_order_release);
    }
protected:
    bool tapActive() const { return tapOn_.load(std::memory_order_acquire); }
    void tapFrame(const QImage &img, qint64 producedNs) {
        std::lock_guard<std::mutex> l(tapMu_);
        if (tap_) tap_(img, producedNs);
    }
    bool reserveFrame() {
        if (inFlight_.load() >= 2) {
            gatedFrames.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        inFlight_.fetch_add(1);
        return true;
    }
private:
    std::atomic<int> inFlight_{0};
    std::mutex tapMu_;
    FrameTap tap_;
    std::atomic<bool> tapOn_{false};

public slots:
    virtual void openDevice() = 0;            // abrir el adaptador (bloqueante)
    virtual void closeDevice() = 0;
    virtual void startScan() = 0;             // barrer 5.8 GHz; al terminar sintoniza el piloto mas fuerte
    virtual void setScanChannels(const QVector<double> &mhz) = 0;  // canales a escanear; vacio = todos
    virtual void watch(double freqMHz) = 0;   // decodificar un piloto
    virtual void stop() = 0;                  // volver a reposo (sin streaming)
    virtual void setWatchRate(int rateSel) { Q_UNUSED(rateSel); }  // 1 = 40 MS/s, 2 = 20 MS/s
    virtual void setGain(double db) { Q_UNUSED(db); }             // < 0 = AGC
    virtual void record(double seconds, const QString &prefix) { Q_UNUSED(seconds); Q_UNUSED(prefix); }

signals:
    void opened(bool ok, const QString &error);
    // 720x480 (NTSC) / 720x576 (PAL), RGBX8888; producedNs = perf_now_ns() al tenerlo el backend
    void frameReady(const QImage &frame, qint64 producedNs);
    void pilotsChanged(const QVector<Pilot> &pilots);
    void statusChanged(const RadioStatus &status);
    void message(const QString &text);        // linea para la barra de estado / log
    void lost(const QString &text);           // adaptador desenchufado: se reconecta solo al volver
};
