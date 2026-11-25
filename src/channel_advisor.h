// Recomendador de canales: a partir de lo que el analizador ha visto en la banda (src/radio: Radio::survey) elige
// canales para N pilotos analogicos y M HDZero que no se pisen entre si ni con lo que ya hay en el aire.
//
// Cada canal candidato se puntua por la interferencia dentro de su ancho (analogico 20 MHz, HDZero 27 MHz, o 17
// en modo estrecho): la potencia media en dB sobre el suelo de ruido mas las rafagas, la fraccion del tiempo que
// lo pisan senales intermitentes (las que van y vienen: WiFi), que la media sola diluye, y las redes WiFi vistas
// (find_wifi) aunque ahora callen, tambien en el resto de su bloque de 80 MHz. Un canal donde ya hay un
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
    float snr_db = 0;   // SNR de su senal (Pilot::snrDb); lo usa el seguimiento de carrera (src/race_monitor.h)
    bool fresh = true;  // medida en este barrido (Pilot::ageS == 0); si no, sigue en el aire pero su snr_db es vieja
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

// Una red WiFi vista en la banda: un canal de 20 MHz de la rejilla WiFi (5000 + 5*n MHz) que se enciende a
// rafagas en su ancho ocupado (OFDM, 52 subportadoras: ~16.6 MHz). Un AP sin trafico solo emite beacons (~0.3 ms
// cada ~102 ms): el tiempo que pisa el canal es casi nulo y la media lo esconde, pero en cuanto alguien usa esa
// red se come el canal. Lo delata el pico reciente, que el barrido guarda y va olvidando (fpv::SurveyGrid).
// Ademas, un AP en 40/80 MHz emite los beacons solo en su canal principal: el resto de su bloque solo se enciende
// con trafico (medido: un AP en el 108 a 80 MHz con un video enciende 5490-5570 entero), y entonces cae encima de
// los canales FPV de ese bloque. Mientras no se le ve trafico no se sabe su ancho: se cuenta su bloque entero; con
// trafico, lo que se enciende es su ancho (cada canal encendido se detecta por si mismo).
struct Wifi {
    int ch = 0;                         // canal WiFi (149)
    double mhz = 0;                     // centro (5745)
    float peak_db = 0;                  // pico reciente tipico en su ancho (dB sobre el suelo)
    float mean_db = 0;                  // media tipica en su ancho (dB sobre el suelo)
    float duty = 0;                     // fraccion del tiempo encendido (media en su ancho)
    double block_lo = 0, block_hi = 0;  // el bloque mas ancho que puede usar en su region (MHz)
    bool traffic = false;               // se le ha visto trafico (mean_db >= kWifiTrafficDb), no solo beacons
    bool now = true;                    // se ve en el ultimo barrido (false: solo la recuerda WifiMemory)
};
constexpr double kWifiOccupied = 16.6;  // MHz que ocupa un canal WiFi de 20 MHz
// Un bin cuenta como encendido por un WiFi si es intermitente (kBurstPeakOverMean) y su pico pasa en kWifiOverNoise
// el pico tipico de la banda (percentil 25 de todos: casi todo es ruido), y nunca de kWifiPeakMin. El ruido tambien
// tiene picos: medido con el adaptador (54 barridos, 5480-5940 MHz), mediana 5.9 dB, 99 % por debajo de 9.3 y
// tramos enteros a 7-8 dB; un AP en el canal 100, 24-27 dB.
constexpr float kWifiPeakMin = 6, kWifiOverNoise = 7;
// Con trafico la media de su ancho sube (medido: 5-24 dB; solo beacons o ruido: ~1 dB)
constexpr float kWifiTrafficDb = 3;
// Bloques WiFi segun la region. UE: no existen el 144 ni el 169-177, asi que el 132-140 llega a 40 MHz (132+136)
// y el 140 y el 165 van solos; EE. UU.: 132-144 y 165-177 (UNII-4) a 80 MHz.
enum class Region { Eu, Us };
// Canales WiFi en los que ha habido rafagas en casi todo su ancho ocupado (en al menos una mitad: un paso del
// barrido mide 20 MHz y los dos lados de un canal pueden caer en pasos distintos, medidos en momentos distintos).
// seen: pilotos en el aire; un "WiFi" cuyo centro tapa uno no cuenta (un piloto recien llegado parece
// intermitente unos barridos, hasta que su media alcanza a su pico), ni uno flojo en las bandas laterales de uno
// fuerte (sideband_reach)
std::vector<Wifi> find_wifi(const std::vector<Bin> &bins, Region region = Region::Eu,
                            const std::vector<Signal> &seen = {});

// Un AP sin trafico se ve a ratos: cada paso del barrido mide 12 ms y hay un beacon cada ~102 ms (~12 % de
// pillarlo), y su pico se olvida en unos barridos. Lo visto se recuerda kWifiHoldS desde la ultima vez, con el
// pico mas alto y si alguna vez tuvo trafico. Lo que tapa un piloto se olvida; lo que cae en las bandas laterales
// de uno fuerte (sideband_reach) no se apunta, pero una red ya vista antes de que se encendiera no se olvida por eso.
constexpr double kWifiHoldS = 300;
class WifiMemory {
   public:
    void reset() { nets_.clear(); }
    void update(const std::vector<Wifi> &now, const std::vector<Signal> &seen, double t_s);
    std::vector<Wifi> known(double t_s) const;  // por canal; now = visto en la ultima actualizacion

   private:
    struct Entry {
        Wifi w;
        double last_s = 0;
    };
    std::vector<Entry> nets_;
    double last_update_s_ = -1;
};

// Canales WiFi seguidos (de 4 en 4) del mismo bloque: una misma red en 40/80 MHz (para mostrarlos juntos)
struct WifiGroup {
    int first = 0, last = 0;
    double lo = 0, hi = 0;  // MHz ocupados (de first a last)
};
std::vector<WifiGroup> group_wifi(const std::vector<Wifi> &wifi);

struct Rating {
    Candidate ch;
    float noise_db = 0;     // interferencia media en su ancho (dB sobre el suelo)
    float burst = 0;        // fraccion del tiempo que lo pisan senales intermitentes (WiFi y similares)
    float coverage = 0;     // fraccion de su ancho que el analizador ha visto
    int busy = -1;          // indice en las senales vistas del piloto que ya lo ocupa, -1 = libre
    int wifi = -1;          // indice en Advice::wifi de la red WiFi que mas le afecta, -1 = ninguna
    bool wifi_block = false;  // esa red no lo pisa: esta en el resto de su bloque (su ancho no se conoce)
    float wifi_cost = 0;    // lo que suma esa red al coste (dB), aunque ahora este callada
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
    bool pinned = false;  // canal fijado por el usuario (advise, pinned): no lo ha elegido la busqueda
};

struct Advice {
    std::vector<Wifi> wifi;              // redes WiFi tenidas en cuenta (Rating::wifi indexa aqui)
    std::vector<Rating> analog, hdzero;  // todos los candidatos puntuados (para la grafica)
    std::vector<Pick> picks;             // la recomendacion, por frecuencia
    bool complete = false;               // false: no caben tantos pilotos con las separaciones minimas
    float worst_db = 0;                  // interferencia del peor canal elegido
};

// Quita del espectro la senal de un piloto propio (marcado como mio en la ventana): no es interferencia. Un VTX
// tiene energia mas alla de su ancho (bandas laterales: medido un bloque de 31 dB a ~12 MHz de un analogico), asi
// que se quita su ancho y, hasta kMaskSkirt MHz mas alla, lo que este 3 dB por encima del nivel de alrededor
// (mediana de un anillo despues de eso). Lo quitado queda a ese nivel, sin rafagas. others: las senales que se ven
// (puede estar la misma s); lo que cae en el ancho mas kMaskSkirt de alguna no entra en el anillo, para que una
// senal pegada no ponga el nivel (si no queda nada del anillo, se usa entero), y del faldon no se quita lo que esta
// mas cerca del centro de otra que del de s (es de la otra).
constexpr double kMaskSkirt = 6, kMaskRing = 5;
void mask_signal(std::vector<Bin> &bins, const Signal &s, const std::vector<Signal> &others = {});

// Un VTX fuerte ensucia mucho mas alla de su ancho. Medido: un analogico en R1 (5658) a 35-39 dB de SNR, muy cerca
// del adaptador, enciende a rafagas de 20 a 40 MHz a cada lado (salian como redes WiFi en los canales 124-128 y
// 136-140, 5620-5700) y, quitado como mio, su canal quedaba a 6-9 dB (sin el VTX, 0.8): el nivel de alrededor
// (el anillo de mask_signal) caia sobre sus propias bandas laterales. Su alcance (MHz mas alla de su ancho) crece
// con su SNR: kSidebandMhzPerDb por dB sobre kSidebandFromDb, entre kMaskSkirt y kSidebandMaxMhz (a 37 dB, 40:
// hasta 50 MHz del centro). Hasta ahi:
//   - mask_signal: si el nivel de alrededor queda kMaskOwnSidebandDb o mas por debajo de su SNR (medido: sus bandas
//     laterales, ~30 dB por debajo) puede ser suya, y se toma el de un anillo mas alla de su alcance si es menor.
//     Mas cerca de su SNR es de otra cosa (una senal ajena pegada que no se ha detectado) y se queda: cuenta. Lo que
//     sobresale entre su faldon y su alcance tambien se queda: no cae en su canal y para un vecino si es interferencia.
//   - find_wifi y WifiMemory no cuentan como WiFi un canal cuyo centro cae en el alcance de una senal al menos
//     kSidebandBelowDb mas fuerte que el (como los hombros entre redes WiFi, kWifiShoulderDb): una red real tan
//     fuerte como para importar junto a un VTX asi sigue viendose, y junto a uno flojo (alcance kMaskSkirt) todo.
// Sin SNR (0) el alcance es kMaskSkirt y nada cuenta como banda lateral.
constexpr float kSidebandFromDb = 10, kSidebandBelowDb = 10, kMaskOwnSidebandDb = 20;
constexpr double kSidebandMhzPerDb = 1.5, kSidebandMaxMhz = 40;
double sideband_reach(const Signal &s);

// Puntuacion de un canal (sin vecinos). wifi: las redes vistas (find_wifi / WifiMemory); cada una suma por estar
// ahi, segun su pico, aunque ahora este callada; si no se le ha visto trafico (ancho desconocido), en el resto de su
// bloque la mitad.
Rating rate(const Candidate &c, const std::vector<Bin> &bins, const std::vector<Signal> &seen,
            const std::vector<Wifi> &wifi = {});

// wifi: las redes a tener en cuenta (la ventana pasa las que recuerda, WifiMemory); nullptr = las de este espectro
// (find_wifi, reglas de la UE)
// pinned: canales fijados por el usuario (ya repartidos a unos pilotos). Entran siempre en la recomendacion
// (Pick::pinned), aunque esten ocupados, sucios o fuera de las bandas elegidas, y la busqueda solo elige los que
// faltan, respetando con ellos las separaciones y la intermodulacion. n_analog y n_hdzero son el total, fijados
// incluidos (con 4 pilotos y 2 fijados busca 2); si hay mas fijados de un sistema que su total, salen todos los
// fijados y ninguno mas de ese sistema. Un fijado se reconoce entre los candidatos por nombre y sistema (no sale
// dos veces). Dos fijados por debajo de la separacion minima: salen los dos y complete = false.
Advice advise(const std::vector<Bin> &bins, const std::vector<Signal> &seen, const std::vector<Candidate> &analog,
              const std::vector<Candidate> &hdzero, int n_analog, int n_hdzero,
              const std::vector<Wifi> *wifi = nullptr, const std::vector<Candidate> &pinned = {});

// Candidatos de las bandas elegidas (letras de kp::fpv_channels(): "ABEFRL"). HDZero: R1-R8, E1, F1, F2, F4,
// L1-L8 (los mismos que src/kp_ext/digital_scan.cpp), con el nombre del canal analogico de esa frecuencia.
std::vector<Candidate> analog_channels(const std::string &bands);
std::vector<Candidate> hdzero_channels(const std::string &bands, bool narrow = false);

// Separaciones (MHz entre centros): minima (por debajo no se elige) e ideal (por debajo se penaliza)
double min_spacing(const Candidate &a, const Candidate &b);
double ideal_spacing(const Candidate &a, const Candidate &b);
double width(const Candidate &c);
double width(const Signal &s);  // ancho ocupado de una senal en el aire (analogica: kAnalogBw)

}  // namespace advisor
