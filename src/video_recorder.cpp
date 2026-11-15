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

void VideoRecorder::run() {
    Result res;
    res.path = path_;
    std::unique_ptr<VideoEncoder> enc;
    VideoEncoder::Params p;
    std::vector<uint8_t> nv12;
    qint64 t0 = 0;
    int64_t lastPts = -1, gaps = 0;
    double encSum = 0, encMax = 0;
    bool failed = false;
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;  // parando y ya vacia
            it = std::move(queue_.front());
            queue_.pop_front();
        }
        if (failed) continue;
        const auto c0 = std::chrono::steady_clock::now();
        if (!enc) {  // primer fotograma: tamano y ritmo
            p.width = it.img.width() & ~1;
            p.height = it.img.height() & ~1;
            if (p.width < 2 || p.height < 2) continue;
            if (p.height > 500) p.fpsNum = 50, p.fpsDen = 1;  // PAL (576 lineas)
            else p.fpsNum = 60000, p.fpsDen = 1001;          // NTSC
            if (aspect_ > 0) {  // forma del pixel = forma de la imagen / forma en pixeles
                const int an = aspect_ > 1.5 ? 16 : 4, ad = aspect_ > 1.5 ? 9 : 3;
                long long n = 1LL * an * p.height, d = 1LL * ad * p.width;
                const long long g = std::gcd(n, d);
                p.sarNum = int(n / g);
                p.sarDen = int(d / g);
            }
            enc = VideoEncoder::create();
            if (!enc || !enc->open(path_, p, &res.error)) {
                if (res.error.isEmpty()) res.error = QStringLiteral("no video encoder");
                failed = true;
                continue;
            }
            nv12.resize(size_t(p.width) * p.height * 3 / 2);
            t0 = it.ns;
        }
        QImage img = it.img;
        if (img.width() < p.width || img.height() < p.height)  // cambio de norma a mitad: al tamano inicial
            img = img.scaled(p.width, p.height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (img.format() != QImage::Format_RGBX8888 && img.format() != QImage::Format_RGBA8888)
            img = img.convertToFormat(QImage::Format_RGBX8888);
        rgbxToNv12(img, p.width, p.height, nv12.data());
        int64_t pts = std::llround(double(it.ns - t0) * p.fpsNum / (double(p.fpsDen) * 1e9));
        if (lastPts >= 0 && pts > lastPts + 1) ++gaps;
        pts = std::max(pts, lastPts + 1);
        if (!enc->write(nv12.data(), pts, &res.error)) {
            failed = true;
            continue;
        }
        lastPts = pts;
        res.frames++;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        encSum += ms;
        encMax = std::max(encMax, ms);
    }
    if (enc) {
        QString err;
        if (!enc->close(&err) && res.error.isEmpty()) res.error = err;
        enc.reset();
    }
    {
        std::lock_guard<std::mutex> l(mu_);
        res.dropped = dropped_;
    }
    res.seconds = lastPts >= 0 ? double(lastPts + 1) * p.fpsDen / p.fpsNum : 0;
    if (res.frames == 0 && res.error.isEmpty()) res.error = QStringLiteral("no picture was received");
    res.ok = res.error.isEmpty();
    if (!res.ok && res.frames == 0) QFile::remove(path_);
    KP_LOGI("video: %s %d fotogramas, %.1f s, %d perdidos por cola llena, %lld huecos del decodificador, "
            "convertir+codificar media %.2f ms max %.1f ms%s%s",
            path_.toUtf8().constData(), res.frames, res.seconds, res.dropped, static_cast<long long>(gaps),
            res.frames ? encSum / res.frames : 0.0, encMax, res.ok ? "" : ", error: ", res.error.toUtf8().constData());
    QMetaObject::invokeMethod(this, [this, res] {
        join();
        running_ = false;
        emit finished(res);
    }, Qt::QueuedConnection);
}
