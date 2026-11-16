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
#include "scan_plan.h"
#include "video_widget.h"  // perf_now_ns()
#include "usb3_switch.h"

namespace {
// Ayuda que se anade al error de apertura. Fuera de #ifdef para que lupdate vea los tres textos.
const char *const kHintWinUsb3 = QT_TRANSLATE_NOOP(
    "RadioBackend",
    "Windows: the adapter has just switched to USB 3 and Windows gave it its Realtek driver again. Install the "
    "WinUSB driver once with File > Install adapter driver: it stays across replugs and USB 2/3 switches. "
    "Then unplug and replug the adapter and press Connect adapter.");
const char *const kHintWin = QT_TRANSLATE_NOOP(
    "RadioBackend",
    "Windows: the adapter needs the WinUSB driver. Install it once with File > Install adapter driver "
    "(administrator permission), then unplug and replug the adapter. Plug it into a USB 3 (blue) port.");
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

void RadioBackend::ensureObjects() {
    if (!poll_) {  // creados en el hilo del backend
        poll_ = new QTimer(this);
        poll_->setInterval(250);
        connect(poll_, &QTimer::timeout, this, &RadioBackend::poll);
        reconnect_ = new QTimer(this);
        reconnect_->setInterval(2000);
        connect(reconnect_, &QTimer::timeout, this, &RadioBackend::tryReconnect);
    }
    if (!radio_) {
        radio_ = std::make_unique<fpv::Radio>([this](const kp::Frame &f) { onFrame(f); });
        radio_->set_scan_plan(scanPlan_);  // seleccion recibida antes de crear la radio
        radio_->set_digital_scan(digitalScan_);
        // Filtro peine de croma NTSC (cmake/kepyur_patches.cmake): menos arcoiris en detalles finos y en el texto
        // del OSD a cambio de ~6 % de un nucleo. En el PC sobra CPU, asi que siempre activo.
        radio_->set_comb(true);
    }
}

QString RadioBackend::openRadio() {
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
        const QString err = QString::fromLocal8Bit(e.what()[0] ? e.what() : "open failed");
        KP_LOGE("open failed: %s", e.what());
        const bool notSupported = err.contains(QLatin1String("NOT_SUPPORTED")) || err.contains(QLatin1String("ACCESS"));
        return err + hint(sw.requested && notSupported);
    }
    poll_->start();
    return {};
}

void RadioBackend::openDevice() {
    ensureObjects();
    reconnect_->stop();  // una conexion manual sustituye a la reconexion automatica
    if (radio_->is_open()) {
        emit opened(true, {});
        return;
    }
    const QString err = openRadio();
    emit opened(err.isEmpty(), err);
    if (err.isEmpty()) poll();
}

void RadioBackend::adapterLost(const QString &mode, double watchMHz) {
    KP_LOGW("adaptador perdido (%s %.0f MHz): esperando a que vuelva", qPrintable(mode), watchMHz);
    poll_->stop();
    radio_->close();
    lastPilotsKey_.clear();
    resumeMode_ = mode;
    resumeMHz_ = watchMHz;
    emit statusChanged(RadioStatus{});
    emit lost(tr("The adapter was disconnected. Plug it back in: it will reconnect by itself."));
    reconnect_->start();
}

void RadioBackend::tryReconnect() {
    if (!kpd::adapter_present()) return;  // aun no esta en el bus: nada que hacer hasta el proximo intento
    reconnect_->stop();
    KP_LOGI("adaptador de nuevo en el bus: reconectando");
    const QString err = openRadio();
    if (!err.isEmpty()) {
        // p.ej. Windows sin WinUSB en ese puerto: se sigue intentando, con el motivo a la vista
        emit lost(tr("Could not reopen the adapter; retrying.") + QStringLiteral("\n\n") + err);
        reconnect_->setInterval(5000);
        reconnect_->start();
        return;
    }
    reconnect_->setInterval(2000);
    emit opened(true, {});
    if (!surveyPlan_.empty()) radio_->survey();
    else if (resumeMode_ == QLatin1String("watch") && resumeMHz_ > 0) radio_->watch(resumeMHz_ * 1e6);
    else if (resumeMode_ == QLatin1String("scan")) radio_->scan();
    emit message(tr("Adapter reconnected"));
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
    if (!surveyPlan_.empty()) radio_->survey();
    else if (mode == QLatin1String("watch") && watchMHz > 0) radio_->watch(watchMHz * 1e6);
    else if (mode == QLatin1String("scan")) radio_->scan();
    poll_->start();
    emit message(tr("Adapter recovered"));
}

void RadioBackend::closeDevice() {
    if (reconnect_) reconnect_->stop();
    if (poll_) poll_->stop();
    if (radio_) radio_->close();
    lastPilotsKey_.clear();
    RadioStatus s;
    emit statusChanged(s);
}

void RadioBackend::startScan() {
    if (!surveyPlan_.empty()) stopSurvey();
    forcePilots_ = true;  // la GUI vacia la lista al escanear: reenviarla aunque salgan los mismos pilotos
    if (radio_ && radio_->is_open()) radio_->scan();
}

void RadioBackend::setScanChannels(const QVector<double> &mhz) {
    scanPlan_ = scan_plan_for(std::vector<double>(mhz.begin(), mhz.end()));
    KP_LOGI("escaneo: %d canal(es) -> %d paso(s)%s", int(mhz.size()), int(scanPlan_.size()),
            scanPlan_.empty() ? " (plan completo)" : "");
    // fpv::Radio lo guarda para los siguientes scan(), tambien tras reabrir (recover/reconexion)
    if (radio_) radio_->set_scan_plan(scanPlan_);
}

void RadioBackend::watch(double freqMHz) {
    if (!surveyPlan_.empty()) stopSurvey();
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

void RadioBackend::setDigitalScan(bool on) {
    digitalScan_ = on;
    KP_LOGI("escaneo: senales digitales (experimental) %s", on ? "si" : "no");
    if (radio_) radio_->set_digital_scan(on);  // cmake/kepyur_patches.cmake, conjunto "digital"
}

void RadioBackend::startSurvey(double loMHz, double hiMHz) {
    surveyPlan_ = survey_plan_for(loMHz, hiMHz);
    KP_LOGI("analizador: %.0f-%.0f MHz, %d paso(s)", loMHz, hiMHz, int(surveyPlan_.size()));
    if (!radio_ || !radio_->is_open()) return;
    radio_->set_scan_plan(surveyPlan_);
    radio_->survey();
}

void RadioBackend::stopSurvey() {
    if (surveyPlan_.empty()) return;
    surveyPlan_.clear();
    KP_LOGI("analizador: parado");
    if (!radio_) return;
    radio_->set_scan_plan(scanPlan_);  // la seleccion de canales del escaneo normal
    if (radio_->is_open()) radio_->idle();
    forcePilots_ = true;
}

void RadioBackend::emitSurvey(const QJsonArray &pilots) {
    SurveyData d;
    std::vector<fpv::SurveyBin> bins = radio_->survey_spectrum(&d.sweeps);
    d.bins.reserve(int(bins.size()));
    for (const auto &b : bins) d.bins.push_back(SpectrumPoint{b.mhz, b.mean_db, b.peak_db, b.duty});
    for (const auto &e : pilots) {
        const QJsonObject p = e.toObject();
        Pilot x;
        x.freqMHz = p.value("mhz").toDouble();
        x.snrDb = p.value("snr").toDouble();
        x.name = p.value("name").toString();
        x.stdName = p.value("std").toString();
        x.ageS = p.value("age_s").toInt();
        x.digital = p.value("digital").toString();
        x.bwMHz = p.value("bw").toDouble();
        d.pilots.push_back(x);
    }
    emit surveyUpdated(d);
}

void RadioBackend::onFrame(const kp::Frame &f) {
    const qint64 t = perf_now_ns();
    const bool tap = tapActive();      // grabando video: este fotograma hace falta aunque la GUI vaya retrasada
    const bool show = reserveFrame();  // la GUI va retrasada: no pintarlo (el siguiente llegara en 16-20 ms)
    if (!tap && !show) return;
    // kp::Frame es R,G,B,0xff por pixel y solo es valido durante la llamada: copiar.
    const QImage img = QImage(f.data, f.width, f.height, f.width * 4, QImage::Format_RGBX8888).copy();
    if (tap) tapFrame(img, t);
    if (show) emit frameReady(img, t);
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

    // Cada reinicio del flujo deja su motivo en last_fault (p.ej. "... [rx dma reset]" cuando wifi-sdr desatasca
    // el RX DMA, cmake/kepyur_patches.cmake): al log una vez, si no el primer fallo de una racha se pierde.
    const QString fault = o.value("last_fault").toString();
    if (!fault.isEmpty() && fault != lastFault_)
        KP_LOGW("radio: fallo del flujo (reinicios %llu): %s", static_cast<unsigned long long>(s.restarts), qPrintable(fault));
    lastFault_ = fault;

    // Desenchufado (o caido del bus): wifi-sdr lo marca como "lost" y ya no vuelve solo
    if (s.open && s.rxState == QLatin1String("lost")) {
        adapterLost(s.mode, s.mode == QLatin1String("watch") ? s.watchMHz : lastWatchMHz_);
        return;
    }
    if (s.mode == QLatin1String("watch") && s.watchMHz > 0) lastWatchMHz_ = s.watchMHz;

    // wifi-sdr se rinde tras 5 fallos seguidos del stream (rx_state "error"). Era sobre todo el atasco del RX DMA
    // del 8822B en algunos controladores USB 3, que ahora wifi-sdr desatasca solo al reiniciar el flujo (parche
    // rxdma); esto queda de ultimo recurso: cerrar y reabrir el adaptador (~10 s). Como mucho una vez cada 15 s.
    if (s.open && s.rxState == QLatin1String("error") && (s.mode == QLatin1String("watch") || s.mode == QLatin1String("scan"))) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastRecoverMs_ > 15000) {
            lastRecoverMs_ = now;
            recover(s.mode, s.watchMHz);
            return;
        }
    }

    // Solo se avisa a la GUI si cambia lo que se muestra. "age_s" sube cada segundo: si entrara en la
    // comparacion, la lista se reconstruiria sin parar (parpadeo, scroll perdido, clics perdidos).
    const QJsonArray pa = o.value("pilots").toArray();
    // Analizador: su propia vista (cada segundo); la lista de pilotos de la ventana principal no se toca
    if (!surveyPlan_.empty() && o.value("survey").toBool()) {
        if (polls_ % 4 == 0) emitSurvey(pa);
        return;
    }
    QVector<Pilot> ps;
    QByteArray key;
    for (const auto &e : pa) {
        const QJsonObject p = e.toObject();
        Pilot x;
        x.freqMHz = p.value("mhz").toDouble();
        x.snrDb = p.value("snr").toDouble();
        x.name = p.value("name").toString();
        x.stdName = p.value("std").toString();
        x.ageS = p.value("age_s").toInt();
        x.digital = p.value("digital").toString();  // cmake/kepyur_patches.cmake, conjunto "digital"
        x.bwMHz = p.value("bw").toDouble();
        key += QStringLiteral("%1|%2|%3|%4|%5;").arg(x.freqMHz, 0, 'f', 1).arg(x.snrDb, 0, 'f', 1).arg(x.name, x.stdName, x.digital).toUtf8();
        ps.push_back(x);
    }
    if (key != lastPilotsKey_ || forcePilots_) {
        lastPilotsKey_ = key;
        forcePilots_ = false;
        emit pilotsChanged(ps);
    }
}
