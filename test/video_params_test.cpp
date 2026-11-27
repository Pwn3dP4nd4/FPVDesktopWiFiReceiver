// Parametros del video grabado segun el primer fotograma (VideoEncoder::paramsFor, src/video_encoder.h): tamano par,
// ritmo NTSC / PAL y forma del pixel para que se vea a 4:3 o 16:9. Sin codificador ni ventana.
//   video_params_test        todos los casos; sale con 1 si alguno falla
#include <cstdio>
#include <string>

#include "video_encoder.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}
}  // namespace

int main() {
    const auto ntsc = VideoEncoder::paramsFor(720, 480, 4.0 / 3);
    check(ntsc.width == 720 && ntsc.height == 480, "NTSC: 720x480");
    check(ntsc.fpsNum == 60000 && ntsc.fpsDen == 1001, "NTSC: 59.94 fotogramas/s");
    check(ntsc.sarNum == 8 && ntsc.sarDen == 9, "NTSC a 4:3: pixel 8:9");

    const auto pal = VideoEncoder::paramsFor(720, 576, 4.0 / 3);
    check(pal.fpsNum == 50 && pal.fpsDen == 1, "PAL: 50 fotogramas/s");
    check(pal.sarNum == 16 && pal.sarDen == 15, "PAL a 4:3: pixel 16:15");

    const auto wide = VideoEncoder::paramsFor(720, 480, 16.0 / 9);
    check(wide.sarNum == 32 && wide.sarDen == 27, "NTSC a 16:9: pixel 32:27");

    const auto odd = VideoEncoder::paramsFor(721, 481, 0);
    check(odd.width == 720 && odd.height == 480, "tamano impar: se queda en par (720x480)");
    check(odd.sarNum == 1 && odd.sarDen == 1, "sin forma de pantalla: pixel cuadrado");

    const auto tiny = VideoEncoder::paramsFor(1, 1, 4.0 / 3);
    check(!VideoEncoder::valid(tiny), "1x1: no hay video que grabar");
    check(VideoEncoder::valid(ntsc), "720x480: si");

    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
