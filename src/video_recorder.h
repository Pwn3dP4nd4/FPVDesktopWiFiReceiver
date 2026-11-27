#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "video_encoder.h"

// DVR: graba en MP4 los fotogramas decodificados. push() se llama desde el hilo del decodificador (derivacion
// Backend::setFrameTap, antes del control de flujo de la GUI) y solo encola; la conversion a YUV y la
// codificacion van en un hilo propio, asi el decodificador nunca espera. Un fotograma cada 1/59,94 s (NTSC) o
// 1/50 (PAL), en modo campos; los ratos sin senal (sin fotogramas) se saltan.
class VideoRecorder : public QObject {
    Q_OBJECT
public:
    struct Result {
        QString path, error;
        bool ok = false;
        bool noPicture = false;  // no llego ningun fotograma (no es un error: no se crea archivo)
        int frames = 0;    // escritos
        int dropped = 0;   // perdidos por ir el codificador retrasado (cola llena)
        double seconds = 0;
    };

    explicit VideoRecorder(QObject *parent = nullptr);
    ~VideoRecorder() override;  // termina el archivo si se estaba grabando

    static bool available();  // esta compilacion lleva codificador
    // displayAspect: forma con la que se ve la imagen (4/3, 16/9; 0 = pixeles cuadrados), va en el MP4.
    bool start(const QString &path, double displayAspect);
    void push(const QImage &frame, qint64 producedNs);  // thread-safe
    void stop();          // asincrono: finished() al terminar el archivo
    void waitFinished();  // bloquea hasta terminar (al cerrar la ventana)
    bool isRecording() const { return running_; }
    qint64 elapsedMs() const { return running_ ? elapsed_.elapsed() : 0; }
    QString path() const { return path_; }

signals:
    void finished(const VideoRecorder::Result &r);

private:
    struct Item {
        QImage img;
        qint64 ns;
    };
    void run();
    // ---- run(), por partes (hilo del grabador) ----
    struct Stats {  // para el log al acabar
        int64_t lastPts = -1, gaps = 0;
        double encSum = 0, encMax = 0;  // ms de convertir y codificar un fotograma
    };
    bool nextItem(Item &it);  // espera el siguiente fotograma; false: parando y la cola vacia
    void abortRecording();    // un error: la grabacion termina en el acto
    // al tamano del video (cambio de norma a mitad) y en RGBX
    static QImage fitFrame(QImage img, const VideoEncoder::Params &p);
    void finish(Result res, const Stats &st, const VideoEncoder::Params &p);  // resultado, log y finished()
    void join();

    QString path_;
    double aspect_ = 4.0 / 3;
    bool running_ = false;  // GUI
    QElapsedTimer elapsed_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool stopping_ = false;
    int dropped_ = 0;
    static constexpr size_t kMaxQueue = 120;  // ~2 s de video (1,4 MB por fotograma)
};
