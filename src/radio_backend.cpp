#include "radio_backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>

#include <exception>
#include <string>

#include "log.h"
#include "radio.h"
#include "usb3_switch.h"

namespace {
// Ayuda que se anade al error de apertura. Fuera de #ifdef para que lupdate vea los tres textos.
const char *const kHintWinUsb3 = QT_TRANSLATE_NOOP(
    "RadioBackend",
    "Windows: the adapter has just switched to USB 3 and Windows sees it as a NEW device (another port), with "
    "the Realtek driver again. Run Zadig again now for this instance (Options > List All Devices > the adapter > "
    "WinUSB > Replace Driver) and press Connect again. This is only needed once per USB port.");
const char *const kHintWin = QT_TRANSLATE_NOOP(
    "RadioBackend",
    "Windows: the adapter needs the WinUSB driver. Open Zadig (https://zadig.akeo.ie), Options > List All "
    "Devices, pick the adapter (USB ID 0BDA B812 or another RTL8812BU), WinUSB > Replace Driver. Plug it into a "
    "USB 3 (blue) port.");
const char *const kHintLinux = QT_TRANSLATE_NOOP(
    "RadioBackend",
    "Linux: the rtw88_8822bu driver must not hold the adapter and your user needs permission: sudo cp "
    "packaging/linux/60-kepyur.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm "
    "trigger, then replug it. Use a USB 3 port.");

QString hint(bool switchedToUsb3) {
#ifdef _WIN32
    const char *h = switchedToUsb3 ? kHintWinUsb3 : kHintWin;
#else
    Q_UNUSED(switchedToUsb3);
    const char *h = kHintLinux;
#endif
    return QStringLiteral("\n\n") + QCoreApplication::translate("RadioBackend", h);
}
}  // namespace

RadioBackend::RadioBackend(QObject *parent) : Backend(parent) {}

RadioBackend::~RadioBackend() = default;  // closeDevice() se llama desde el hilo del backend al salir

void RadioBackend::openDevice() {
    if (!poll_) {  // creado en el hilo del backend
        poll_ = new QTimer(this);
        poll_->setInterval(250);
        connect(poll_, &QTimer::timeout, this, &RadioBackend::poll);
    }
    if (!radio_) radio_ = std::make_unique<kp::Radio>([this](const kp::Frame &f) { onFrame(f); });
    if (radio_->is_open()) {
        emit opened(true, {});
        return;
    }

    // Igual que la app Android: el 8812BU arranca en USB 2 tras enchufarlo; pedir el cambio a USB 3
    // (registros en RAM, nada permanente) y esperar a que se re-enumere. 40 MS/s no cabe en USB 2.
    emit message(tr("Looking for the adapter..."));
    const auto sw = kpd::request_usb3_first_adapter();
    if (sw.requested) {
        emit message(tr("Switching the adapter to USB 3..."));
        kpd::wait_adapter(10000);
    }

    emit message(tr("Starting the chip (~5 s)..."));
    try {
        radio_->open(-1);
    } catch (const std::exception &e) {
        QString err = QString::fromLocal8Bit(e.what()[0] ? e.what() : "open failed");
        KP_LOGE("open failed: %s", e.what());
        const bool notSupported = err.contains(QLatin1String("NOT_SUPPORTED")) || err.contains(QLatin1String("ACCESS"));
        emit opened(false, err + hint(sw.requested && notSupported));
        return;
    }
    poll_->start();
    emit opened(true, {});
    poll();
}

void RadioBackend::recover(const QString &mode, double watchMHz) {
    KP_LOGW("receptor en error (%s): reabriendo el adaptador", qPrintable(mode));
    emit message(tr("Receiver stuck: reopening the adapter..."));
    poll_->stop();
    radio_->close();
    try {
        radio_->open(-1);
    } catch (const std::exception &e) {
        KP_LOGE("reopen failed: %s", e.what());
        emit opened(false, QString::fromLocal8Bit(e.what()) + hint(false));
        return;
    }
    if (mode == QLatin1String("watch") && watchMHz > 0) radio_->watch(watchMHz * 1e6);
    else if (mode == QLatin1String("scan")) radio_->scan();
    poll_->start();
    emit message(tr("Adapter recovered"));
}

void RadioBackend::closeDevice() {
    if (poll_) poll_->stop();
    if (radio_) radio_->close();
    lastPilotsJson_.clear();
    RadioStatus s;
    emit statusChanged(s);
}

void RadioBackend::startScan() {
    if (radio_ && radio_->is_open()) radio_->scan();
}

void RadioBackend::watch(double freqMHz) {
    if (radio_ && radio_->is_open()) radio_->watch(freqMHz * 1e6);
}

void RadioBackend::stop() {
    if (radio_ && radio_->is_open()) radio_->idle();
}

void RadioBackend::setWatchRate(int rateSel) {
    if (radio_) radio_->set_watch_rate(rateSel);
}

void RadioBackend::setGain(double db) {
    if (radio_) radio_->set_gain(db);
}

void RadioBackend::record(double seconds, const QString &prefix) {
    if (radio_) radio_->record(seconds, prefix.toStdString());
}

void RadioBackend::onFrame(const kp::Frame &f) {
    if (!reserveFrame()) return;  // la GUI va retrasada: descartar (el siguiente llegara en 16-20 ms)
    // kp::Frame es R,G,B,0xff por pixel y solo es valido durante la llamada: copiar.
    QImage img(f.data, f.width, f.height, f.width * 4, QImage::Format_RGBX8888);
    emit frameReady(img.copy());
}

void RadioBackend::poll() {
    if (!radio_) return;
    const QByteArray js = QByteArray::fromStdString(radio_->status_json());
    if (++polls_ % 20 == 0) KP_LOGD("status %s", js.constData());  // cada 5 s con --verbose
    const QJsonObject o = QJsonDocument::fromJson(js).object();
    RadioStatus s;
    s.open = o.value("open").toBool();
    s.usb3 = o.value("usb3").toBool();
    s.chip = o.value("chip").toString();
    s.mode = o.value("mode").toString();
    s.rxState = o.value("rx_state").toString();
    s.rxError = o.value("rx_error").toString();
    s.watchMHz = o.value("watch_hz").toDouble() / 1e6;
    s.msps = o.value("watch_msps").toDouble();
    s.fallback = o.value("fallback").toBool();
    s.snrDb = o.contains("snr_db") ? o.value("snr_db").toDouble() : -1;
    s.scanStep = o.value("scan_step").toInt();
    s.scanSteps = o.value("scan_steps").toInt();
    const QJsonObject v = o.value("video").toObject();
    s.locked = v.value("locked").toBool();
    s.lock = v.value("lock").toDouble();
    s.videoStd = v.value("std").toString();
    s.fps = v.value("fps").toDouble();
    s.colour = v.value("colour").toBool();
    s.dropped = static_cast<quint64>(o.value("dropped").toDouble());
    s.restarts = static_cast<quint64>(o.value("restarts").toDouble());
    emit statusChanged(s);

    // wifi-sdr se rinde tras 5 fallos seguidos del stream (rx_state "error"). En Windows ocurre a veces tras
    // varios reinicios rapidos; cerrar y reabrir el adaptador lo recupera. Como mucho una vez cada 15 s.
    if (s.open && s.rxState == QLatin1String("error") && (s.mode == QLatin1String("watch") || s.mode == QLatin1String("scan"))) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastRecoverMs_ > 15000) {
            lastRecoverMs_ = now;
            recover(s.mode, s.watchMHz);
            return;
        }
    }

    const QJsonArray pa = o.value("pilots").toArray();
    const QByteArray pj = QJsonDocument(pa).toJson(QJsonDocument::Compact);
    if (pj != lastPilotsJson_) {
        lastPilotsJson_ = pj;
        QVector<Pilot> ps;
        for (const auto &e : pa) {
            const QJsonObject p = e.toObject();
            Pilot x;
            x.freqMHz = p.value("mhz").toDouble();
            x.snrDb = p.value("snr").toDouble();
            x.name = p.value("name").toString();
            x.stdName = p.value("std").toString();
            x.ageS = p.value("age_s").toInt();
            ps.push_back(x);
        }
        emit pilotsChanged(ps);
    }
}
