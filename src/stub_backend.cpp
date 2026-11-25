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
    survey_->start(race_ ? 200 : hi < 3000 ? 250 : 1000);  // 2,4 GHz: un tercio de la banda cada 250 ms, como el de verdad
    surveyTick();
}

void StubBackend::stopSurvey() {
    // siempre responde (como RadioBackend::stopSurvey): la ventana cuenta sus paradas
    if (survey_) survey_->stop();
    emit surveyStopped();
}

// Escenario de carrera: ruido, los cuatro pilotos del reparto (R1, R3, R6, R8, SNR 20-25 dB; R8 se pierde un
// barrido de cada 7), desde el barrido 5 un VTX ajeno en 5748 MHz, encima de R3 (que baja su SNR), y desde el 8 otro
// en 5858, encima de R6: dos recomendaciones a la vez
void StubBackend::raceTick(SurveyData &d) {
    const int k = d.sweeps;
    const bool intruder = k >= 5;
    struct Vtx {
        double mhz;
        float snr;
        const char *name;
        bool on;
    };
    const Vtx vtx[] = {{5658, 23, "R1", true},
                       {5732, intruder ? 15.f : 22.f, "R3", true},
                       {5843, 24, "R6", true},
                       {5917, 21, "R8", k % 7 != 3},
                       {5748, 18, "", intruder},
                       {5858, 17, "", k >= 8}};
    for (double f = surveyLo_ + 0.078; f < surveyHi_; f += 0.15625) {
        SpectrumPoint p{f, 0.4f + float((qHash(qint64(f * 1000)) % 100) / 100.0), 3.f, 0.f};
        for (const Vtx &v : vtx) {
            const double df = std::abs(f - v.mhz);
            if (!v.on || df >= 6) continue;
            const float db = v.snr - float(df) * 2 + float((qHash(qint64(k * 7 + v.mhz)) % 20) / 10.0);
            if (db > p.meanDb) p.meanDb = p.peakDb = db, p.duty = 1;
        }
        p.seq = quint32(k + 1);  // un barrido por actualizacion: todo recien medido
        d.bins.push_back(p);
    }
    for (const Vtx &v : vtx) {
        // la SNR baila un poco de un barrido a otro; un piloto perdido sigue en la lista unos segundos
        const double snr = v.snr + ((qHash(qint64(k * 13 + v.mhz)) % 30) / 10.0 - 1.5);
        if (v.on) d.pilots.push_back(Pilot{v.mhz, snr, QString::fromLatin1(v.name), QString(), 0});
        else if (v.name[0]) d.pilots.push_back(Pilot{v.mhz, 0, QString::fromLatin1(v.name), QString(), 1});
    }
}

// Banda de 2,4 GHz simulada (analizador ELRS): un AP con trafico en el canal WiFi 1, otro mas fuerte en el 6, uno
// callado (solo beacons) en el 11, rafagas cortas de Bluetooth por toda la banda y ruido. Un barrido por actualizacion.
void StubBackend::elrsTick(SurveyData &d) {
    // un tercio de la banda por actualizacion (seq de cada bin: la ultima que paso por su tercio), un barrido cada tres
    const int tick = d.sweeps;
    const int k = tick / 3;
    d.sweeps = k;
    const double seen = std::min(1.0, 0.4 + 0.2 * k);
    for (double f = surveyLo_ + 0.078; f < surveyHi_; f += 0.15625) {
        const quint32 h = qHash(qint64(f * 1000) + k * 7919);
        SpectrumPoint p{f, 0.3f + float((h % 100) / 120.0), 4.f + float((h % 50) / 10.0), 0.f};
        struct Ap {
            double mhz;
            float mean, peak, duty;
        };
        const Ap aps[] = {{2412, 6, 24, 0.22f}, {2437, 12, 32, 0.45f}, {2462, 0.5f, 18, 0.004f}};
        for (const Ap &a : aps)
            if (std::abs(f - a.mhz) < 8.3) {
                p.meanDb = std::max(p.meanDb, a.mean * float(seen));
                p.peakDb = std::max(p.peakDb, a.peak * float(seen));
                p.duty = std::max(p.duty, a.duty);
            }
        const quint32 hb = qHash(qint64(std::floor(f)) * 31 + 5);  // Bluetooth: un MHz entero de cada tres
        if (hb % 3 == 0) p.peakDb = std::max(p.peakDb, 15.f + float(hb % 6)), p.duty = std::max(p.duty, 0.01f);
        const int third = std::clamp(int(3 * (f - surveyLo_) / (surveyHi_ - surveyLo_)), 0, 2);
        const int last = tick - (((tick - third) % 3) + 3) % 3;
        p.seq = last >= 0 ? quint32(last + 1) : 0;
        d.bins.push_back(p);
    }
}

void StubBackend::surveyTick() {
    SurveyData d;
    d.sweeps = sweeps_++;
    if (surveyHi_ < 3000) {
        elrsTick(d);
        emit surveyUpdated(d);
        return;
    }
    if (race_) {
        raceTick(d);
        emit surveyUpdated(d);
        return;
    }
    // el barrido va por tercios de la zona, uno por actualizacion (como el de verdad: ~3 s, espectro cada s): seq de
    // cada bin, la ultima actualizacion que paso por su tercio (0 = aun no); barridos completos, uno cada tres
    const int t = sweeps_;
    d.sweeps = t / 3;
    const double seen = std::min(1.0, 0.3 + 3.0 * d.sweeps / memory_);  // fraccion de rafagas captadas
    for (double f = surveyLo_ + 0.078; f < surveyHi_; f += 0.15625) {
        SpectrumPoint p{f, 0.4f + float((qHash(qint64(f * 1000)) % 100) / 100.0), 3.f, 0.f};
        const int third = std::clamp(int(3 * (f - surveyLo_) / (surveyHi_ - surveyLo_)), 0, 2);
        const int last = t - 1 - (((t - 1 - third) % 3) + 3) % 3;
        p.seq = last >= 0 ? quint32(last + 1) : 0;
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
