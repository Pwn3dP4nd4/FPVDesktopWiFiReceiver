#include "video_recorder.h"

#include <QFile>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

#include "log.h"
#include "video_encoder.h"

namespace {

// RGBX8888 (R,G,B,x) -> NV12, BT.601 rango limitado (video SD), croma promediado en bloques 2x2.
void rgbxToNv12(const QImage &img, int w, int h, uint8_t *out) {
    uint8_t *yp = out, *uv = out + w * h;
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = img.constScanLine(y), *r1 = img.constScanLine(y + 1);
        uint8_t *y0 = yp + y * w, *y1 = y0 + w, *c = uv + (y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int sr = 0, sg = 0, sb = 0;
            for (const auto &[row, dst] : {std::pair{r0, y0}, std::pair{r1, y1}}) {
                for (int k = 0; k < 2; ++k) {
                    const uint8_t *p = row + (x + k) * 4;
                    const int r = p[0], g = p[1], b = p[2];
                    dst[x + k] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    sr += r;
                    sg += g;
                    sb += b;
                }
            }
            sr = (sr + 2) >> 2;
            sg = (sg + 2) >> 2;
            sb = (sb + 2) >> 2;
            c[x] = uint8_t(((-38 * sr - 74 * sg + 112 * sb + 128) >> 8) + 128);
            c[x + 1] = uint8_t(((112 * sr - 94 * sg - 18 * sb + 128) >> 8) + 128);
        }
    }
}

}  // namespace

VideoRecorder::VideoRecorder(QObject *parent) : QObject(parent) { qRegisterMetaType<VideoRecorder::Result>(); }

VideoRecorder::~VideoRecorder() {
    stop();
    join();
}

bool VideoRecorder::available() {
    static const bool ok = VideoEncoder::available();
    return ok;
}

bool VideoRecorder::start(const QString &path, double displayAspect) {
    if (running_ || !available()) return false;
    join();
    path_ = path;
    aspect_ = displayAspect;
    {
        std::lock_guard<std::mutex> l(mu_);
        queue_.clear();
        stopping_ = false;
        dropped_ = 0;
    }
    running_ = true;
    elapsed_.start();
    thread_ = std::thread([this] { run(); });
    return true;
}

void VideoRecorder::push(const QImage &frame, qint64 producedNs) {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (stopping_) return;
        if (queue_.size() >= kMaxQueue) {
            ++dropped_;
            return;
        }
        queue_.push_back({frame, producedNs ? producedNs : qint64(std::chrono::steady_clock::now().time_since_epoch().count())});
    }
    cv_.notify_one();
}

void VideoRecorder::stop() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stopping_ = true;
    }
    cv_.notify_one();
}

void VideoRecorder::waitFinished() {
    stop();
    join();
}

void VideoRecorder::join() {
    if (thread_.joinable()) thread_.join();
}

bool VideoRecorder::nextItem(Item &it) {
    std::unique_lock<std::mutex> l(mu_);
    cv_.wait(l, [this] { return stopping_ || !queue_.empty(); });
    if (queue_.empty()) return false;  // parando y ya vacia
    it = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void VideoRecorder::abortRecording() {
    // Un error (no arranca el codificador, disco lleno...) termina la grabacion en el acto: la GUI se entera por
    // finished() y deja de mostrar REC, en vez de seguir "grabando" sin guardar nada hasta que se pulse parar.
    std::lock_guard<std::mutex> l(mu_);
    stopping_ = true;
    queue_.clear();
}

QImage VideoRecorder::fitFrame(QImage img, const VideoEncoder::Params &p) {
    if (img.width() < p.width || img.height() < p.height || img.width() > p.width + 1 ||
        img.height() > p.height + 1)  // cambio de norma a mitad (NTSC <-> PAL): al tamano inicial
        img = img.scaled(p.width, p.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (img.format() != QImage::Format_RGBX8888 && img.format() != QImage::Format_RGBA8888)
        img = img.convertToFormat(QImage::Format_RGBX8888);
    return img;
}

void VideoRecorder::finish(Result res, const Stats &st, const VideoEncoder::Params &p) {
    {
        std::lock_guard<std::mutex> l(mu_);
        res.dropped = dropped_;
    }
    res.seconds = st.lastPts >= 0 ? double(st.lastPts + 1) * p.fpsDen / p.fpsNum : 0;
    res.noPicture = res.frames == 0 && res.error.isEmpty();
    res.ok = res.error.isEmpty() && !res.noPicture;
    if (!res.ok && res.frames == 0) QFile::remove(path_);
    KP_LOGI("video: %s %d fotogramas, %.1f s, %d perdidos por cola llena, %lld huecos sin senal saltados, "
            "convertir+codificar media %.2f ms max %.1f ms%s%s",
            path_.toUtf8().constData(), res.frames, res.seconds, res.dropped, static_cast<long long>(st.gaps),
            res.frames ? st.encSum / res.frames : 0.0, st.encMax, res.ok ? "" : ", error: ",
            res.error.toUtf8().constData());
    QMetaObject::invokeMethod(this, [this, res] {
        join();
        running_ = false;
        emit finished(res);
    }, Qt::QueuedConnection);
}

void VideoRecorder::run() {
    Result res;
    res.path = path_;
    std::unique_ptr<VideoEncoder> enc;
    VideoEncoder::Params p;
    std::vector<uint8_t> nv12;
    Stats st;
    qint64 prevNs = 0;
    Item it;
    while (nextItem(it)) {
        const auto c0 = std::chrono::steady_clock::now();
        if (!enc) {  // primer fotograma: tamano y ritmo
            p = VideoEncoder::paramsFor(it.img.width(), it.img.height(), aspect_);
            if (!VideoEncoder::valid(p)) continue;
            enc = VideoEncoder::create();
            if (!enc || !enc->open(path_, p, &res.error)) {
                if (res.error.isEmpty()) res.error = QStringLiteral("no video encoder");
                enc.reset();
                abortRecording();
                break;
            }
            nv12.resize(size_t(p.width) * p.height * 3 / 2);
        }
        rgbxToNv12(fitFrame(it.img, p), p.width, p.height, nv12.data());
        // pts consecutivos: un rato sin senal (el decodificador no entrega fotogramas) se salta en vez de quedar
        // congelado. Asi el tiempo del MP4 es siempre el numero de fotograma, que es lo que alinea los fotogramas
        // clave con los trozos del MP4 fragmentado (video_encoder_mf.cpp). Los huecos solo se cuentan para el log.
        if (st.lastPts >= 0 && double(it.ns - prevNs) * p.fpsNum / (double(p.fpsDen) * 1e9) > 1.5) ++st.gaps;
        prevNs = it.ns;
        const int64_t pts = st.lastPts + 1;
        if (!enc->write(nv12.data(), pts, &res.error)) {
            abortRecording();
            break;
        }
        st.lastPts = pts;
        res.frames++;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        st.encSum += ms;
        st.encMax = std::max(st.encMax, ms);
    }
    if (enc) {
        QString err;
        if (!enc->close(&err) && res.error.isEmpty()) res.error = err;
        enc.reset();
    }
    finish(res, st, p);
}
