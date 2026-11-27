// Grabacion de video (src/video_recorder.cpp) con el codificador del sistema y fotogramas sinteticos, sin adaptador:
// un MP4 con los fotogramas pedidos, un rato sin senal que se salta y una grabacion sin ningun fotograma.
//   video_recorder_test        todos los casos; sale con 1 si alguno falla (sin codificador: no hay nada que probar)
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <string>

#include "video_recorder.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// graba los fotogramas que da frames(i) (720x480, cada 1/59,94 s mas los huecos de gapAt) y espera a finished()
VideoRecorder::Result record(const QString &path, int n, int gapAt = -1) {
    VideoRecorder rec;
    VideoRecorder::Result out;
    QEventLoop loop;
    QObject::connect(&rec, &VideoRecorder::finished, &loop, [&](const VideoRecorder::Result &r) {
        out = r;
        loop.quit();
    });
    if (!rec.start(path, 4.0 / 3)) return out;
    const qint64 frameNs = 1001000000LL / 60;
    qint64 ns = 1;
    for (int i = 0; i < n; i++) {
        QImage img(720, 480, QImage::Format_RGBX8888);
        img.fill(QColor::fromHsv((i * 7) % 360, 200, 200));
        if (i == gapAt) ns += 60 * frameNs;  // un segundo sin senal
        rec.push(img, ns);
        ns += frameNs;
    }
    rec.stop();
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);  // no deberia tardar tanto
    loop.exec();
    return out;
}
}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    qRegisterMetaType<VideoRecorder::Result>();
    if (!VideoRecorder::available()) {
        std::printf("sin codificador de video en esta compilacion: nada que probar\n");
        return 0;
    }
    QTemporaryDir dir;
    {
        const QString path = dir.filePath(QStringLiteral("rec.mp4"));
        const auto r = record(path, 90, 45);
        check(r.ok && r.error.isEmpty(), "90 fotogramas: sin error");
        check(r.frames == 90, "90 fotogramas: los 90 escritos");
        check(r.seconds > 1.4 && r.seconds < 1.6, "el segundo sin senal se salta: ~1,5 s de video");
        check(QFileInfo(path).size() > 1000, "el MP4 existe y tiene algo");
    }
    {
        const QString path = dir.filePath(QStringLiteral("empty.mp4"));
        const auto r = record(path, 0);
        check(r.noPicture && r.frames == 0 && r.error.isEmpty(), "sin fotogramas: noPicture, sin error");
        check(!QFileInfo::exists(path), "sin fotogramas: no queda archivo");
    }
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
