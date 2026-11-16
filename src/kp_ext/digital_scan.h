// Senales digitales (HDZero y otras) en el escaneo. Propio de FPVDesktopWiFiReceiver: fpv::Radio (src/radio) lo
// llama desde el hilo DSP en cada paso del escaneo, con Radio::set_digital_scan(). No decodifica nada: solo dice que hay una senal digital, su centro y su ancho.
//
// Que se busca: un bloque ancho y plano en el espectro (OFDM: todas las subportadoras con la misma potencia,
// bordes casi verticales), continuo en el tiempo (HDZero emite sin parar; el WiFi va a rafagas), de 12 a 31 MHz.
// El video analogico en FM no es plano (su espectro es el histograma de niveles de la imagen, con picos en el
// sincronismo y el nivel de negro) y ademas pasa antes por la comprobacion de sincronismo de kepyur.
//
// Cada paso del escaneo ve +-16.5 MHz y los pasos van cada 20 MHz: una senal de 27 MHz casi nunca cabe entera
// en uno. Si un paso la ve cortada por un borde y el paso anterior la vio cortada por el otro, se unen.
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace kpx {

using cf = std::complex<float>;

// Bloque ancho y plano en el espectro de un paso (frecuencias relativas al centro de ese paso).
struct WideRegion {
    double f0_hz = 0, f1_hz = 0;            // banda ocupada, a suelo + 6 dB
    bool clip_lo = false, clip_hi = false;  // llega al borde del margen util: la senal sigue mas alla
    float snr_db = 0;                       // nivel mediano del bloque sobre el suelo de ruido
    float flat = 0;                         // fraccion del bloque a menos de 3 dB de su nivel mediano
    float duty = 0;                         // fraccion de rodajas de ~0.1 ms en las que el bloque esta presente
};

// db: espectro en dB con fftshift (kp::Psd::db()), fs: muestreo, edge_hz: margen util (+-) alrededor del centro.
std::vector<WideRegion> find_wide_regions(const std::vector<float> &db, double fs, double edge_hz);

// Continuidad en el tiempo de la banda [f0, f1] (relativa al centro) en las muestras x.
float band_duty(const cf *x, size_t n, double fs, double f0_hz, double f1_hz);

enum class Kind { None, HDZero, HDZeroNarrow, Other };
// Que parece por ancho y frecuencia: HDZero (27 MHz, "wide") o HDZero estrecho (17 MHz, 540p60) si esta
// centrado en un canal de HDZero; si no, "otra senal digital" (DJI, Walksnail...).
Kind classify(double centre_hz, double width_hz);
const char *kind_code(Kind k);  // "hdzero" / "hdzero_narrow" / "digital" (para status_json)
// Canales de HDZero (MHz): R1-R8, E1, F1, F2, F4 y banda baja L1-L8.
const std::vector<double> &hdzero_channels_mhz();

struct DigitalSignal {
    double centre_hz = 0, width_hz = 0;  // absolutos
    float snr_db = 0;
    Kind kind = Kind::None;
};

// Estado del escaneo: un analyse() y un step_done() por paso, en orden; reset() al empezar cada escaneo.
class DigitalScan {
public:
    void reset();
    void analyse(const std::vector<float> &db, const cf *x, size_t n, double fs, double edge_hz);
    // Senales completas en este paso, o uniendo este paso con el anterior. centre_hz: centro real del paso.
    std::vector<DigitalSignal> step_done(double centre_hz);

private:
    struct Abs {
        double f0, f1;
        bool clip_lo, clip_hi;
        float snr, flat, duty;
    };
    std::vector<WideRegion> cur_;
    std::vector<Abs> prev_;
    double prevCentre_ = 0, edge_ = 16.5e6;
    bool havePrev_ = false;
};

}  // namespace kpx
