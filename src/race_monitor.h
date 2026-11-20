// Seguimiento de carrera: con los canales ya fijados (repartidos a los pilotos, advisor::advise con pinned), el
// analizador sigue barriendo durante la carrera y aqui se va apuntando, barrido a barrido, como le va a cada canal
// fijado: la SNR de su piloto, los barridos en que se le pierde, las senales ajenas que se le meten encima y la
// interferencia que se ve en su canal (con la senal de su propio piloto quitada, advisor::mask_signal). Con eso se
// estima que pilotos han tenido peor video y, si a alguno de ellos le iria claramente mejor en otro canal libre
// (compatible con los demas fijados), se recomienda moverlo.
//
// Lo que no se puede ver: la interferencia que cae justo debajo de la portadora de un piloto que esta emitiendo
// (se quita con su senal). Se ve en los barridos en que ese piloto no esta, en las redes WiFi recordadas
// (advisor::WifiMemory) y en lo que asoma fuera de su ancho. Una SNR baja con el canal limpio no se arregla
// cambiando de canal (antena, potencia, distancia): ahi no se recomienda mover.
//
// Sin Qt: la ventana (src/channel_analyzer.cpp) le pasa cada barrido; test/race_monitor_test.cpp lo prueba con
// espectros sinteticos.
#pragma once
#include <optional>
#include <vector>

#include "channel_advisor.h"

namespace race {

// Una senal esta en un canal fijado si es del mismo sistema (analogica / digital) y su centro esta a menos de esto
constexpr double kMatchMhz = 5;
// Penalizaciones (dB) por la fraccion de barridos con el problema (todo el tiempo: esto)
constexpr float kIntruderCost = 20;  // una senal ajena encima de su canal
constexpr float kLostCost = 20;      // su piloto no se ve (corte), entre la primera y la ultima vez que se vio
// Para recomendar mover: barridos minimos observados y mejora minima (dB) del canal nuevo sobre el actual
constexpr int kMinSweeps = 10;
constexpr float kMoveMinGainDb = 3;

struct ChannelHealth {
    advisor::Candidate ch;   // el canal fijado
    int sweeps = 0;          // barridos observados desde start() (o desde que se movio, moved())
    int seen = 0;            // barridos en los que se ha visto a su piloto
    int lost = 0;            // barridos sin su piloto entre la primera y la ultima vez que se vio (los del
                             // principio y los del final no cuentan: aun no habia salido o ya ha aterrizado)
    float snr_med = 0;       // SNR de su piloto (dB): mediana de los barridos en que se vio
    float snr_low = 0;       // percentil 10 (los peores momentos); 0 si no se ha visto nunca
    int intruders = 0;       // barridos con una senal ajena (no casa con ningun fijado) solapando su canal
    float channel_cost = 0;  // media por barrido de advisor::rate(canal, bins con los fijados quitados, {}, wifi).cost
    float penalty = 0;       // lo atribuible al canal: channel_cost + kIntruderCost * intruders / sweeps
    float quality = 0;       // estimacion del video (dB, mas es mejor):
                             //   snr_low - penalty - kLostCost * lost / (barridos entre la primera y la ultima vez)
};

enum class Cause {
    Intruder,      // una senal ajena se le mete encima en mas del 20 % de los barridos
    Interference,  // ruido, rafagas o WiFi en su canal
};

struct Move {
    int from = -1;             // indice en health() del piloto a mover
    advisor::Candidate to;     // canal nuevo (del mismo sistema)
    float gain_db = 0;         // health()[from].penalty - coste del canal nuevo (advisor::Rating::cost)
    Cause cause = Cause::Interference;
};

class Monitor {
   public:
    // Empieza (o vuelve a empezar) con estos canales fijados; borra lo apuntado
    void start(const std::vector<advisor::Candidate> &pinned);
    // Un barrido. bins: el espectro tal cual (con las senales de los pilotos dentro); now: lo que se ve en ESTE
    // barrido (Pilot::ageS == 0), con su snr_db, fijados y ajenos mezclados; wifi: las redes a tener en cuenta
    // (WifiMemory::known)
    void update(const std::vector<advisor::Bin> &bins, const std::vector<advisor::Signal> &now,
                const std::vector<advisor::Wifi> &wifi);
    // El piloto de health()[i] se ha cambiado al canal c: lo suyo vuelve a empezar, lo de los demas sigue
    void moved(int i, const advisor::Candidate &c);

    const std::vector<ChannelHealth> &health() const { return health_; }
    int sweeps() const { return sweeps_; }
    // Indice del de peor quality entre los que se han visto alguna vez (seen > 0); -1 si ninguno
    int worst() const;
    // Recomendar mover a uno de los de peor video: entre los vistos alguna vez, se mira, de peor a mejor quality,
    // la mitad peor de los fijados (redondeando hacia arriba), y de ellos los que lleven al menos kMinSweeps
    // barridos; para cada uno, su mejor canal nuevo es el que elige
    // advisor::advise con los demas fijados (candidatos de su sistema, sin el suyo actual ni los que esten a menos
    // de kMatchMhz de el), con el ultimo espectro (todos los fijados quitados) y las senales ajenas del ultimo
    // barrido como ocupadas. Se recomienda el primero cuya mejora sea al menos kMoveMinGainDb.
    std::optional<Move> suggest(const std::vector<advisor::Candidate> &analog,
                                const std::vector<advisor::Candidate> &hdzero) const;

   private:  // (se puede cambiar libremente: la interfaz es lo de arriba)
    std::vector<ChannelHealth> health_;
    std::vector<std::vector<float>> snr_;   // SNR de cada barrido en que se vio, por fijado
    std::vector<int> first_, last_;          // barrido (indice local) de la primera / ultima vez que se vio, -1
    std::vector<double> cost_sum_;           // suma de channel_cost
    std::vector<advisor::Bin> last_bins_;    // ultimo espectro, con los fijados vistos quitados
    std::vector<advisor::Signal> foreign_;   // senales ajenas del ultimo barrido
    std::vector<advisor::Wifi> wifi_;        // redes del ultimo barrido
    int sweeps_ = 0;

    void reset(size_t i);  // borra lo apuntado del fijado i (conserva su canal)
};

}  // namespace race
