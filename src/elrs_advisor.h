#pragma once
// Analizador de 2,4 GHz para ELRS: lo que dice el espectro acumulado (fpv::SurveyBin, dB sobre el suelo) de los 80
// canales de ELRS (ISM2G4: 2400.4..2479.4 MHz, de 1 en 1), las redes WiFi que hay (canales 1-13) y otras emisiones, y
// que configuracion de ELRS aguanta mejor eso. Sin Qt: los textos los pone la ventana (src/elrs_analyzer.cpp).
//
// ELRS salta por los 80 canales en un orden pseudoaleatorio (de la frase de enlace) y no deja quitar ninguno: no se
// puede "elegir canal" como con el video. Lo que se puede elegir es como de robusto es cada paquete (tasa de paquetes y
// modulacion: LoRa, FLRC, DVDA, Gemini), la potencia y donde se ponen las antenas. Por eso lo que importa es que parte
// de los saltos cae en un momento ocupado: la media de la ocupacion de los 80 canales.
#include <vector>

namespace elrs {

struct Bin {
    double mhz = 0;
    float mean_db = 0, peak_db = 0, duty = 0;
};

constexpr int kChannels = 80;
constexpr double kFirstMhz = 2400.4, kSpacingMhz = 1.0;
inline double channel_mhz(int i) { return kFirstMhz + kSpacingMhz * i; }

// Un canal de ELRS cuenta como ocupado en un instante si alguna senal pasa kOnDb sobre el suelo (la duty del barrido,
// fpv::SurveyGrid::kDutyOver: 6 dB). Ruido del adaptador: pico mediano ~6 dB, 99 % bajo 9.3 dB (src/channel_advisor.h)
constexpr float kNoisePeakDb = 10;  // pico por encima de esto: hay algo
constexpr float kStrongDb = 35;     // un transmisor muy cerca (puede ensordecer al receptor)
constexpr float kWifiStrongDb = 20; // una red WiFi asi de fuerte esta cerca (quiza es tuya)

enum class Level { Unknown, Clean, Busy, Bad };

struct Channel {
    int index = 0;
    double mhz = 0;
    bool measured = false;
    float mean_db = 0, peak_db = 0;  // en su ancho (~0.8 MHz), medianas, dB sobre el suelo
    float duty = 0;                  // fraccion del tiempo ocupado, sin la del ruido (kNoiseDuty en el .cpp)
    int wifi = -1;                             // indice en Report::wifi de la red que lo pisa, -1 = ninguna
    Level level = Level::Unknown;
};

struct Wifi {
    int ch = 0;           // 1..13
    double mhz = 0;       // centro (2412 + 5 (ch-1))
    float mean_db = 0;    // media en su ancho (sube con trafico)
    float peak_db = 0;    // pico tipico en su ancho
    float duty = 0;       // fraccion del tiempo encendida
    bool traffic = false; // trafico (no solo beacons)
};

// Otras emisiones que no son WiFi
enum class Other {
    Hoppers,     // muchos picos estrechos y cortos por toda la banda: Bluetooth, otras emisoras (ELRS de otros pilotos)
    Continuous,  // una senal que no calla (duty ~1) fuera de un WiFi: video analogico de 2,4 GHz, camara inalambrica
    Strong,      // algo muy fuerte muy cerca: puede saturar el receptor de ELRS
};
struct Emission {
    Other kind = Other::Hoppers;
    double lo_mhz = 0, hi_mhz = 0;
    float peak_db = 0;
    float share = 0;  // Hoppers: parte de los canales con esos picos
};

enum class Verdict { Unknown, Clean, Moderate, Heavy };
enum class Use { Racing, Freestyle, LongRange };
// Dominio regulatorio de 2,4 GHz del firmware (ExpressLRS src/lib/FHSS/FHSS.cpp): "ISM2G4", o "CE_LBT" si se compila
// con Regulatory_Domain_EU_CE_2400 (src/lib/LBT/LBT.cpp: escucha antes de transmitir por encima de 10 mW)
enum class Region { Fcc, EuLbt };

// Modos de 2,4 GHz (SX1280) de ExpressLRS, de src/src/common.cpp (ExpressLRS_AirRateConfig / AirRateRFperf) y de
// https://www.expresslrs.org/info/signal-health/: cada cuanto sale un paquete, cuanto ocupa el aire, cuantas veces se
// manda cada paquete (DVDA) y en cuantas frecuencias distintas (https://www.expresslrs.org/software/gemini/: "D500
// sends 2 repeat packets on 2 different frequencies", D250 "4 repeat packets across 3 frequencies")
struct Mode {
    const char *name;
    bool flrc;
    int interval_us;
    int toa_us;
    int sends;        // envios de cada paquete
    int freqs;        // frecuencias distintas entre esos envios
    int sensitivity;  // dBm
};
enum class ModeId { F1000, F500, D500, D250, Hz500, Hz333Full, Hz250, Hz150, Hz100Full, Hz50, Count };
const Mode &mode(ModeId m);

// Otro piloto con ELRS de 2,4 GHz a la vez (mismo modo que tu, peor caso): parte de tus envios que coinciden en el
// tiempo con uno suyo en el mismo canal. Dos paquetes se pisan si se solapan (ventana: los dos tiempos en el aire) y
// caen en el mismo de los 80 canales. Cota alta: si tu senal llega mas fuerte, el tuyo suele pasar igual.
float pilot_collision(const Mode &m);
// Paquetes que se pierden (estimacion): cada envio cae ocupado con probabilidad hit (lo medido) o lo pisa uno de
// `others` pilotos; con DVDA se pierde si caen ocupados los envios de todas sus frecuencias distintas.
float packet_loss(const Mode &m, float hit, int others);

// Lo que se sugiere, de mas a menos importante. La ventana pone el texto de cada uno (con los datos de Report)
enum class Tip {
    RateClean,        // banda limpia: la tasa mas rapida para el uso
    RateModerate,     // algo ocupada: LoRa antes que FLRC, o DVDA
    RateHeavy,        // muy ocupada: DVDA / tasa mas lenta / Gemini
    Gemini,           // TX Gemini + receptor true diversity: el mismo paquete en dos frecuencias a la vez
    Pilots,           // varios pilotos: frase de enlace propia, apagar las emisoras que no vuelan
    DynamicPower,     // potencia dinamica (necesita telemetria: no con Off ni Race)
    LbtBusy,          // CE_LBT con la banda ocupada: se salta envios con el canal ocupado
    OwnWifi,          // una red WiFi fuerte: si es tuya (router, movil, gafas, portatil), apagala o a 5 GHz
    WifiOnTx,         // la WiFi del propio ELRS (TX, receptor sin enlace, backpack): solo para actualizar (siempre)
    Hoppers,          // otras emisoras que saltan (Bluetooth, otros enlaces)
    Continuous,       // emision continua: buscar la fuente (video 2,4, camara), alejarse
    Strong,           // muy fuerte cerca: separar antenas del RX del VTX / del emisor
    Antennas,         // antenas del receptor (practica habitual, no de la documentacion de ELRS)
    Telemetry,        // ratio de telemetria: Race en carreras, Std si no
    Band900,          // banda imposible: ELRS 900 MHz o GemX (900 + 2,4, hardware LR1121)
};

struct Report {
    std::vector<Channel> channels;  // los 80
    std::vector<Wifi> wifi;         // de mas a menos fuerte
    std::vector<Emission> other;
    int measured = 0, clean = 0, busy = 0, bad = 0;
    float hit = 0;        // fraccion media del tiempo ocupada (en los medidos): parte de los saltos que caen ocupados
    float worst_peak = 0;
    int others = 0;       // otros pilotos con ELRS de 2,4 GHz a la vez (lo dice el usuario)
    float pilot_hit = 0;  // parte de los envios que pisa alguno de ellos (a 500 Hz; pilot_collision)
    Verdict verdict = Verdict::Unknown;
    std::vector<Tip> tips;
    std::vector<ModeId> modes;  // los modos a comparar para el uso elegido (la ventana pone su perdida estimada)
};

constexpr int kManyPilots = 5;  // con 6-8 pilotos la documentacion recomienda Gemini (software/gemini)

Report analyse(const std::vector<Bin> &bins, Use use, Region region, int others = 0);
// Las sugerencias de un Report segun su veredicto (analyse ya las pone; la ventana las rehace con el veredicto que
// muestra, que no cambia a cada barrido), en el orden en que se muestran
std::vector<Tip> tips_for(const Report &r, Region region);
// Posicion de una sugerencia en ese orden (para juntar las de ahora con las que se siguen mostrando un rato)
int tip_order(Tip t);

}  // namespace elrs
