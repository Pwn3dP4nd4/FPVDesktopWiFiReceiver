// Recomendador de canales: a partir de lo que el analizador ha visto en la banda (src/radio: Radio::survey) elige
// canales para N pilotos analogicos y M HDZero que no se pisen entre si ni con lo que ya hay en el aire.
//
// Cada canal candidato se puntua por la interferencia dentro de su ancho (analogico 20 MHz, HDZero 27 MHz, o 17
// en modo estrecho): la potencia media en dB sobre el suelo de ruido mas las rafagas, la fraccion del tiempo que
// lo pisan senales intermitentes (las que van y vienen: WiFi), que la media sola diluye. Un canal donde ya hay un
// piloto (analogico o digital) queda ocupado. Entre los canales elegidos: separacion minima (por debajo,
// imposible) e ideal (por debajo, se penaliza), y los productos de intermodulacion de 3er orden (2*f1 - f2) de
// cada par de analogicos que caen sobre otro canal elegido.
// La busqueda es voraz (el mejor siguiente canal) desde varios puntos de partida y luego intercambios de un canal
// hasta que ninguno mejora el total.
#pragma once
#include <string>
#include <vector>

namespace advisor {

struct Bin {  // fpv::SurveyBin
    double mhz = 0;
    float mean_db = 0, peak_db = 0, duty = 0;
};

struct Signal {  // un piloto visto en el aire
    double mhz = 0;
    double bw_mhz = 0;  // ancho ocupado; analogico: 0 (se toma kAnalogBw)
    bool digital = false;
    std::string name;   // "R4", "" si no coincide con ninguno
};

struct Candidate {
    std::string name;  // "R1"
    double mhz = 0;
    bool hdzero = false;
    double bw_mhz = 0;  // ancho; 0 = el de su sistema (kAnalogBw, kHdzBw)
};

constexpr double kAnalogBw = 20, kHdzBw = 27;  // MHz que ocupa cada sistema
// HDZero en modo estrecho: unos 17 MHz segun lo que publica HDZero (aqui no se ha medido; el ancho de los que
// estan en el aire si se mide, src/kp_ext/digital_scan.cpp, y llega en Signal::bw_mhz)
constexpr double kHdzNarrowBw = 17;
// Un bin cuenta como rafaga si su pico reciente supera su media en esto (dB): constante = piloto, no rafaga
constexpr float kBurstPeakOverMean = 6;

struct Rating {
    Candidate ch;
    float noise_db = 0;     // interferencia media en su ancho (dB sobre el suelo)
    float burst = 0;        // fraccion del tiempo que lo pisan senales intermitentes (WiFi y similares)
    float coverage = 0;     // fraccion de su ancho que el analizador ha visto
    int busy = -1;          // indice en las senales vistas del piloto que ya lo ocupa, -1 = libre
    float cost = 0;         // lo que se minimiza (dB)
};

enum class Note {
    TooClose,   // dos canales elegidos por debajo de la separacion ideal
    Imd,        // un producto de intermodulacion cae sobre otro canal elegido
    Busy,       // no habia canales libres suficientes: se uso uno ocupado
    Partial,    // parte del canal no se ha analizado (banda no seleccionada)
};

struct Pick {
    Rating r;
    std::vector<Note> notes;
};

struct Advice {
    std::vector<Rating> analog, hdzero;  // todos los candidatos puntuados (para la grafica)
    std::vector<Pick> picks;             // la recomendacion, por frecuencia
    bool complete = false;               // false: no caben tantos pilotos con las separaciones minimas
    float worst_db = 0;                  // interferencia del peor canal elegido
};

// Quita del espectro la senal de un piloto propio (marcado como mio en la ventana): no es interferencia. Un VTX
// tiene energia mas alla de su ancho (bandas laterales: medido un bloque de 31 dB a ~12 MHz de un analogico), asi
// que se quita su ancho y, hasta kMaskSkirt MHz mas alla, lo que este 3 dB por encima del nivel de alrededor
// (mediana de un anillo despues de eso). Lo quitado queda a ese nivel, sin rafagas.
constexpr double kMaskSkirt = 6, kMaskRing = 5;
void mask_signal(std::vector<Bin> &bins, const Signal &s);

// Puntuacion de un canal (sin vecinos)
Rating rate(const Candidate &c, const std::vector<Bin> &bins, const std::vector<Signal> &seen);

Advice advise(const std::vector<Bin> &bins, const std::vector<Signal> &seen, const std::vector<Candidate> &analog,
              const std::vector<Candidate> &hdzero, int n_analog, int n_hdzero);

// Candidatos de las bandas elegidas (letras de kp::fpv_channels(): "ABEFRL"). HDZero: R1-R8, E1, F1, F2, F4,
// L1-L8 (los mismos que src/kp_ext/digital_scan.cpp), con el nombre del canal analogico de esa frecuencia.
std::vector<Candidate> analog_channels(const std::string &bands);
std::vector<Candidate> hdzero_channels(const std::string &bands, bool narrow = false);

// Separaciones (MHz entre centros): minima (por debajo no se elige) e ideal (por debajo se penaliza)
double min_spacing(const Candidate &a, const Candidate &b);
double ideal_spacing(const Candidate &a, const Candidate &b);
double width(const Candidate &c);

}  // namespace advisor
