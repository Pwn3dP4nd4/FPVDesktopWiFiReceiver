#include "stub_backend.h"

#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "video_widget.h"  // perf_now_ns()

StubBackend::StubBackend(QObject *parent) : Backend(parent) {}

void StubBackend::ensureTimer() {
    // Se crea en el hilo del backend (primer slot llamado), no en el constructor.
    if (timer_) return;
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &StubBackend::tick);
}

void StubBackend::openDevice() {
    ensureTimer();
    emit opened(true, {});
    emit message(tr("Demo mode (no adapter)"));
}

void StubBackend::closeDevice() { stop(); }

void StubBackend::startScan() {
    ensureTimer();
    emit message(tr("Scanning (simulated)..."));
    QVector<Pilot> p;
    // R2: una senal HDZero (digital: sale en la lista pero no tiene imagen)
    for (const Pilot &f : {Pilot{5769, 18.5, "R4", "NTSC", 0}, Pilot{5695, 14.2, "R2", "", 0, "hdzero", 27.1},
                           Pilot{5806, 11.2, "R5", "PAL", 0}}) {
        if (f.isDigital() && !digital_) continue;
        bool in = scanMHz_.isEmpty();
        for (double m : scanMHz_) in = in || qAbs(m - f.freqMHz) <= 10;
        if (in) p.push_back(f);
    }
    emit pilotsChanged(p);
    if (!p.isEmpty()) watch(p.front().freqMHz);
}

void StubBackend::watch(double f) {
    ensureTimer();
    watching_ = f;
    timer_->start(16);
}

void StubBackend::stop() {
    if (timer_) timer_->stop();
    watching_ = 0;
    RadioStatus s;
    s.open = true;
    s.mode = QStringLiteral("idle");
    s.chip = QStringLiteral("demo");
    emit statusChanged(s);
}

// Analizador simulado: un WiFi a rafagas en 5650-5690 MHz (canales 132/136), un AP sin trafico en el canal WiFi
// 165 (solo beacons: pico alto, casi nunca encendido), un piloto analogico en R4, un HDZero en R6 y algo de ruido.
// Las rafagas se van descubriendo barrido a barrido, como con el adaptador.
void StubBackend::startSurvey(double lo, double hi) {
    if (!survey_) {
        survey_ = new QTimer(this);
        connect(survey_, &QTimer::timeout, this, &StubBackend::surveyTick);
    }
    stop();
    surveyLo_ = lo;
    surveyHi_ = hi;
    sweeps_ = 0;
    survey_->start(1000);
    surveyTick();
}

void StubBackend::stopSurvey() {
    if (!survey_ || !survey_->isActive()) return;
    survey_->stop();
    emit surveyStopped();
}

void StubBackend::surveyTick() {
    SurveyData d;
    d.sweeps = sweeps_++;
    const double seen = std::min(1.0, 0.3 + 3.0 * sweeps_ / memory_);  // fraccion de rafagas captadas
    for (double f = surveyLo_ + 0.078; f < surveyHi_; f += 0.15625) {
        SpectrumPoint p{f, 0.4f + float((qHash(qint64(f * 1000)) % 100) / 100.0), 3.f, 0.f};
        if (f > 5650 && f < 5690) {
            p.meanDb = float(3 + 2 * seen);
            p.peakDb = float(30 * seen);
            p.duty = 0.2f;
        }
        if (std::abs(f - 5825) < 8.3) {  // beacons del canal 165
            p.peakDb = float(20 * seen);
            p.duty = 0.003f;
        }
        if (std::abs(f - 5769) < 6) p.meanDb = p.peakDb = 22 - float(std::abs(f - 5769)) * 2, p.duty = 1;
        if (std::abs(f - 5845) < 13.5) p.meanDb = p.peakDb = 25, p.duty = 1;  // HDZero en R6
        d.bins.push_back(p);
    }
    d.pilots.push_back(Pilot{5769, 20, "R4", "NTSC", 0});
    d.pilots.push_back(Pilot{5843, 24, "R6", "", 0, "hdzero", 27});
    emit surveyUpdated(d);
}

void StubBackend::tick() {
    // R5 (5806 MHz) simula un canal sin senal: no llegan fotogramas, como al apagarse un VTX. R2 (5695 MHz) es
    // HDZero: tampoco hay fotogramas (no se decodifica), pero si senal.
    const bool hdzero = qAbs(watching_ - 5695) < 1;
    const bool noSignal = hdzero || qAbs(watching_ - 5806) < 1;
    if (noSignal) {
        if (++n_ % 15 == 0) {
            RadioStatus s;
            s.open = true;
            s.chip = QStringLiteral("demo");
            s.usb3 = true;
            s.mode = QStringLiteral("watch");
            s.watchMHz = watching_;
            s.msps = hdzero ? 40 : 20;
            s.snrDb = hdzero ? 14 : 2;
            s.videoStd = hdzero ? QString() : QStringLiteral("PAL");
            emit statusChanged(s);
        }
        return;
    }
    QImage img(720, 480, QImage::Format_RGBX8888);
    QPainter p(&img);
    static const QColor c[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue, Qt::black};
    for (int i = 0; i < 8; ++i) p.fillRect(i * 90, 0, 90, 480, c[i]);
    p.fillRect((n_ * 6) % 720, 200, 40, 80, Qt::gray);
    p.end();
    const qint64 t = perf_now_ns();
    if (tapActive()) tapFrame(img, t);
    if (reserveFrame()) emit frameReady(img, t);
    if (++n_ % 15 == 0) {
        RadioStatus s;
        s.open = true;
        s.chip = QStringLiteral("demo");
        s.usb3 = true;
        s.mode = QStringLiteral("watch");
        s.watchMHz = watching_;
        s.msps = 40;
        s.snrDb = 18.5;
        s.locked = true;
        s.lock = 0.97;
        s.videoStd = QStringLiteral("NTSC");
        s.fps = 60;
        s.colour = true;
        emit statusChanged(s);
    }
}
