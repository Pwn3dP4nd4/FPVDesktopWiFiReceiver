#include "stub_backend.h"

#include <QPainter>
#include <QTimer>

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
    emit message(QStringLiteral("Modo demo (sin adaptador)"));
}

void StubBackend::closeDevice() { stop(); }

void StubBackend::startScan() {
    ensureTimer();
    emit message(QStringLiteral("Escaneando (simulado)..."));
    QVector<Pilot> p;
    p.push_back({5769, 18.5, "R4", "NTSC", 0});
    p.push_back({5806, 11.2, "R5", "PAL", 0});
    emit pilotsChanged(p);
    watch(5769);
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

void StubBackend::tick() {
    QImage img(720, 480, QImage::Format_RGBX8888);
    QPainter p(&img);
    static const QColor c[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue, Qt::black};
    for (int i = 0; i < 8; ++i) p.fillRect(i * 90, 0, 90, 480, c[i]);
    p.fillRect((n_ * 6) % 720, 200, 40, 80, Qt::gray);
    p.end();
    if (reserveFrame()) emit frameReady(img);
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
