// Compilado sin codificador de video (Linux sin las cabeceras de FFmpeg): el boton de grabar sale desactivado.
#include "video_encoder.h"

std::unique_ptr<VideoEncoder> VideoEncoder::create() { return nullptr; }
QString VideoEncoder::backendName() { return QString(); }
