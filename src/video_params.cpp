#include "video_encoder.h"

#include <numeric>

VideoEncoder::Params VideoEncoder::paramsFor(int frameW, int frameH, double displayAspect) {
    Params p;
    p.width = frameW & ~1;
    p.height = frameH & ~1;
    if (p.height > 500) p.fpsNum = 50, p.fpsDen = 1;  // PAL (576 lineas)
    else p.fpsNum = 60000, p.fpsDen = 1001;          // NTSC
    if (displayAspect > 0 && valid(p)) {  // forma del pixel = forma de la imagen / forma en pixeles
        const int an = displayAspect > 1.5 ? 16 : 4, ad = displayAspect > 1.5 ? 9 : 3;
        const long long n = 1LL * an * p.height, d = 1LL * ad * p.width;
        const long long g = std::gcd(n, d);
        p.sarNum = int(n / g);
        p.sarDen = int(d / g);
    }
    return p;
}
