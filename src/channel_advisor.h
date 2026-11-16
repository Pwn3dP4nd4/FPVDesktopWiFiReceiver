// Recomendador de canales: a partir de lo que el analizador ha visto en la banda (src/radio: Radio::survey) elige
// canales para N pilotos analogicos y M HDZero que no se pisen entre si ni con lo que ya hay en el aire.
//
// Cada canal candidato se puntua por la interferencia dentro de su ancho (analogico 20 MHz, HDZero 27 MHz): la
// potencia media mas las rafagas (pico por la fraccion de tiempo que esta encendido; asi pesa un WiFi que va y
// viene), en dB sobre el suelo de ruido. Un canal donde ya hay un piloto (analogico o digital) queda ocupado.
// Entre los canales elegidos: separacion minima (por debajo, imposible) e ideal (por debajo, se penaliza), y en
// analogico los productos de intermodulacion de 3er orden (2*f1 - f2) que caen sobre otro canal elegido.
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
};

constexpr double kAnalogBw = 20, kHdzBw = 27;  // MHz que ocupa cada sistema

struct Rating {
    Candidate ch;
    float noise_db = 0;     // interferencia media en su ancho (dB sobre el suelo)
    float burst = 0;        // fraccion de tiempo con rafagas fuertes (WiFi y similares)
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

// Puntuacion de un canal (sin vecinos)
Rating rate(const Candidate &c, const std::vector<Bin> &bins, const std::vector<Signal> &seen);

Advice advise(const std::vector<Bin> &bins, const std::vector<Signal> &seen, const std::vector<Candidate> &analog,
              const std::vector<Candidate> &hdzero, int n_analog, int n_hdzero);

// Candidatos de las bandas elegidas (letras de kp::fpv_channels(): "ABEFRL"). HDZero: R1-R8, E1, F1, F2, F4,
// L1-L8 (los mismos que src/kp_ext/digital_scan.cpp), con el nombre del canal analogico de esa frecuencia.
std::vector<Candidate> analog_channels(const std::string &bands);
std::vector<Candidate> hdzero_channels(const std::string &bands);

// Separaciones (MHz entre centros): minima (por debajo no se elige) e ideal (por debajo se penaliza)
double min_spacing(bool a_hdz, bool b_hdz);
double ideal_spacing(bool a_hdz, bool b_hdz);

}  // namespace advisor
