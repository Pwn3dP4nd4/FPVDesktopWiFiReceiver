#pragma once
#include <QString>

#include <cstdint>
#include <memory>

// Codificador H.264 + contenedor MP4 del sistema: Media Foundation en Windows (sin DLL extra; usa el
// codificador por hardware si lo hay), libavcodec/libavformat en Linux (opcional al compilar).
// Una instancia por archivo; todas las llamadas desde el mismo hilo (el del grabador).
class VideoEncoder {
public:
    struct Params {
        int width = 0, height = 0;    // pares
        int fpsNum = 60000, fpsDen = 1001;  // tiempo en fotogramas: pts * fpsDen / fpsNum segundos
        int sarNum = 1, sarDen = 1;   // forma del pixel (720x480 a 4:3 = 8:9)
        int bitrate = 8000000;        // bit/s
    };
    virtual ~VideoEncoder() = default;
    virtual bool open(const QString &path, const Params &p, QString *err) = 0;
    // Fotograma NV12 (Y de width*height, luego UV entrelazado de width*height/2). pts en fotogramas, creciente.
    virtual bool write(const uint8_t *nv12, int64_t pts, QString *err) = 0;
    virtual bool close(QString *err) = 0;  // termina el archivo (indice MP4); sin esto no se puede abrir

    static std::unique_ptr<VideoEncoder> create();  // nullptr si esta compilacion no puede grabar
    static bool available() { return create() != nullptr; }
    static QString backendName();  // para el log

    // Para un video de fotogramas frameW x frameH que se ve a displayAspect (ancho / alto; <= 0: pixel cuadrado):
    // tamano par, NTSC (59.94/s) o PAL (50/s, mas de 500 lineas) y la forma del pixel (src/video_params.cpp)
    static Params paramsFor(int frameW, int frameH, double displayAspect);
    static bool valid(const Params &p) { return p.width >= 2 && p.height >= 2; }
};
