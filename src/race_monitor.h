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
// cambiando de canal (antena, potencia, distancia): ahi no se recomienda mover. Mientras el piloto de un fijado no
// ha salido, una senal ajena del mismo sistema a menos de kMatchMhz de su canal se toma por el (con dos, la mas
// cercana es la suya y la otra un intruso).
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
// Una senal digital ajena (DJI, Walksnail, HDZero...) a menos de esto del borde de un canal analogico cuenta como
// intruso aunque no lo pise: un receptor FM se come el derrame de un OFDM pegado a su canal, y lo que cae dentro
// del canal queda tapado con la senal de su propio piloto (advisor::mask_signal), asi que no se veria de otro modo
constexpr double kDigitalGuardMhz = 5;
// Penalizaciones (dB) por la fraccion de barridos con el problema (todo el tiempo: esto)
constexpr float kIntruderCost = 20;  // una senal ajena encima de su canal
constexpr float kLostCost = 20;      // su piloto no se ve (corte), entre la primera y la ultima vez que se vio
// Para recomendar mover: barridos minimos observados y mejora minima (dB) del canal nuevo sobre el actual
constexpr int kMinSweeps = 10;
constexpr float kMoveMinGainDb = 3;
// Una recomendacion que ya se esta mostrando (prev de suggest_all) se mantiene mientras su mejora no baje de esto:
// con la mejora rondando kMoveMinGainDb, la medida de cada barrido la hacia aparecer y desaparecer (medido con un VTX
// en R1: "-> R2 +3 dB" y "sin canal mejor" alternando)
constexpr float kMoveKeepGainDb = 2;
// suggest tiene en cuenta las senales ajenas vistas en estos ultimos barridos (una que sale a ratos tambien)
constexpr int kForeignMemory = 10;
// El canal que deja un piloto movido (moved): su senal ahi sigue siendo suya (no ajena) hasta que no se la ve en
// kLeaveSweeps barridos seguidos (ya ha cambiado) o, como mucho, kLeaveMaxSweeps barridos
constexpr int kLeaveSweeps = 3, kLeaveMaxSweeps = 120;

struct ChannelHealth {
    advisor::Candidate ch;   // el canal fijado
    int sweeps = 0;          // barridos observados desde start() (o desde que se movio, moved())
    int seen = 0;            // barridos en los que se ha visto a su piloto
    int lost = 0;            // barridos sin su piloto entre la primera y la ultima vez que se vio (los del
                             // principio y los del final no cuentan: aun no habia salido o ya ha aterrizado)
    float snr_med = 0;       // SNR de su piloto (dB): mediana de los barridos en que se vio
    float snr_low = 0;       // percentil 10 (los peores momentos); 0 si no se ha visto nunca
    int intruders = 0;       // barridos con una senal ajena (no casa con ningun fijado) solapando su canal
    float channel_cost = 0;  // media por barrido de advisor::rate(canal, bins, {}, wifi).cost, con las senales de
                             // los fijados y las ajenas que lo pisan quitadas (esas ya cuentan en intruders)
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

// Cuantos de estos canales fijados tienen a su piloto en now (una senal del mismo sistema a menos de kMatchMhz; cada
// senal cuenta para uno solo, el mas cercano). Para el auto-inicio de la carrera (esperar a que salgan N pilotos) y
// su fin automatico (ya no queda ninguno)
int present(const std::vector<advisor::Candidate> &pinned, const std::vector<advisor::Signal> &now);

// Mis pilotos (marcados en el analizador, sin canal fijado) que ya estan en un canal recomendado: por cada pick (en su
// orden), los indices de mine que estan en el (mismo sistema, a menos de kMatchMhz). Los fijados no cuentan: su
// piloto ya es suyo. Los que no salen en ninguno tienen que cambiar de canal
std::vector<std::vector<int>> already_on(const std::vector<advisor::Pick> &picks,
                                         const std::vector<advisor::Signal> &mine);

class Monitor {
   public:
    // Empieza (o vuelve a empezar) con estos canales fijados; borra lo apuntado
    void start(const std::vector<advisor::Candidate> &pinned);
    // Un barrido. bins: el espectro tal cual (con las senales de los pilotos dentro); now: lo que esta en el aire,
    // con su snr_db, fijados y ajenos mezclados (el analizador pasa los vistos hace poco, kRaceHoldS en
    // src/channel_analyzer.cpp: los pilotos se miden menos a menudo que el espectro); wifi: las redes a tener en
    // cuenta (WifiMemory::known)
    void update(const std::vector<advisor::Bin> &bins, const std::vector<advisor::Signal> &now,
                const std::vector<advisor::Wifi> &wifi);
    // El piloto de health()[i] se ha cambiado al canal c: lo suyo vuelve a empezar, lo de los demas sigue. Hasta
    // que aterrice y cambie, su senal en el canal viejo es suya (leaving()), no la de un intruso
    void moved(int i, const advisor::Candidate &c);
    // Canales que acaba de dejar un piloto movido y en los que aun se le ve (o se le vio hace menos de kLeaveSweeps)
    const std::vector<advisor::Candidate> &leaving() const { return leaving_ch_; }

    const std::vector<ChannelHealth> &health() const { return health_; }
    int sweeps() const { return sweeps_; }
    // Indice del de peor quality entre los que se han visto alguna vez (seen > 0); -1 si ninguno
    int worst() const;
    // Recomendaciones de mover, una por canal fijado que la necesite: entre los vistos alguna vez, de peor a mejor
    // quality (los de prev primero), los que lleven al menos kMinSweeps barridos; para cada uno, su mejor canal nuevo
    // es el que elige advisor::advise con los demas fijados (con las recomendaciones anteriores de esta lista ya
    // aplicadas: se pueden aplicar todas a la vez) y candidatos de su sistema, sin el suyo actual ni los que esten a
    // menos de kMatchMhz de el ni a menos de advisor::min_spacing de una senal ajena o de un canal de leaving() (su
    // piloto aun esta ahi); con el ultimo espectro (todos
    // los fijados quitados) y las senales ajenas de los ultimos kForeignMemory barridos como ocupadas. Sale si la
    // mejora es al menos kMoveMinGainDb. prev: las recomendaciones que se estan mostrando; la de un piloto se
    // mantiene si sigue valiendo (su canal nuevo sigue libre y compatible, mejora de al menos kMoveKeepGainDb), salvo
    // que otro canal mejore al menos kMoveMinGainDb mas: no cambia de un barrido a otro por poco.
    std::vector<Move> suggest_all(const std::vector<advisor::Candidate> &analog,
                                  const std::vector<advisor::Candidate> &hdzero,
                                  const std::vector<Move> &prev = {}) const;
    // La primera de suggest_all (la del peor video, o la de prev si sigue valiendo)
    std::optional<Move> suggest(const std::vector<advisor::Candidate> &analog,
                                const std::vector<advisor::Candidate> &hdzero, const Move *prev = nullptr) const;

   private:  // (se puede cambiar libremente: la interfaz es lo de arriba)
    std::vector<ChannelHealth> health_;
    std::vector<std::vector<float>> snr_;   // SNR de cada barrido en que se vio, por fijado
    std::vector<int> first_, last_;          // barrido (indice local) de la primera / ultima vez que se vio, -1
    std::vector<double> cost_sum_;           // suma de channel_cost
    std::vector<advisor::Bin> last_bins_;    // ultimo espectro, con los fijados vistos quitados
    std::vector<advisor::Signal> foreign_;   // senales ajenas del ultimo barrido
    struct Recent {
        advisor::Signal s;
        int sweep;  // ultimo barrido (sweeps_) en que se vio
    };
    std::vector<Recent> recent_;             // ajenas de los ultimos kForeignMemory barridos
    std::vector<advisor::Wifi> wifi_;        // redes del ultimo barrido
    struct Leaving {
        advisor::Candidate ch;
        int since, seen;  // barrido (sweeps_) en que se dejo y ultimo en que se vio su senal ahi
    };
    std::vector<Leaving> leaving_;
    std::vector<advisor::Candidate> leaving_ch_;  // los canales de leaving_ (leaving())
    int sweeps_ = 0;

    void reset(size_t i);  // borra lo apuntado del fijado i (conserva su canal)
    // ---- update(), por pasos ----
    // por fijado, el indice en now de la senal de su piloto (-1: no esta)
    std::vector<int> match_pilots(const std::vector<advisor::Signal> &now) const;
    // por senal de now, si es de un piloto propio (de un fijado o de uno movido que aun esta en su canal viejo)
    std::vector<bool> own_signals(const std::vector<advisor::Signal> &now, const std::vector<int> &pilot);
    void remember_foreign();  // foreign_ -> recent_
    void update_health(size_t i, int pilot, const std::vector<advisor::Signal> &now,
                       const std::vector<advisor::Wifi> &wifi);
    // ---- suggest_all(), por pasos ----
    std::vector<int> move_order(const std::vector<Move> &prev) const;  // a quien se busca canal, por orden
    // los canales de pool a los que se puede mover el piloto de h
    std::vector<advisor::Candidate> free_channels(const ChannelHealth &h, const std::vector<advisor::Candidate> &pool,
                                                  const std::vector<advisor::Signal> &foreign) const;
    // la recomendacion para el fijado i con el reparto chans (las anteriores aplicadas); nada si no compensa
    std::optional<Move> suggest_for(int i, const std::vector<advisor::Candidate> &chans,
                                    const std::vector<advisor::Candidate> &cand,
                                    const std::vector<advisor::Signal> &foreign, const std::vector<Move> &prev) const;
    struct Choice {  // el canal al que mover a un piloto y lo que gana (gain < 0: ninguno)
        advisor::Candidate to;
        float gain = -1;
        bool kept = false;  // es la recomendacion anterior, que se mantiene
    };
    static bool fits(const advisor::Candidate &c, const advisor::Candidate &from,
                     const std::vector<advisor::Candidate> &others);
    std::optional<advisor::Rating> best_channel(const advisor::Candidate &from,
                                                const std::vector<advisor::Candidate> &others,
                                                const std::vector<advisor::Candidate> &cand,
                                                const std::vector<advisor::Signal> &foreign) const;
    void keep_previous(int i, const std::vector<advisor::Candidate> &others, const std::vector<advisor::Candidate> &cand,
                       const std::vector<advisor::Signal> &foreign, const std::vector<Move> &prev, Choice &c) const;
};

}  // namespace race
