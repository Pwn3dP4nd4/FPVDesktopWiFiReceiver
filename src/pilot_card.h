#pragma once
#include <QString>

#include "analyzer_style.h"  // kLowSnrDb
#include "backend.h"         // Pilot

// La tarjeta de un piloto en la lista de la ventana principal: que dice y de que color es su SNR. Lo pinta
// PilotDelegate (src/pilot_delegate.h); aqui, sin widgets, lo que se puede probar.
namespace pilotcard {

// SNR del video: por debajo de kLowSnrDb, flojo (el mismo umbral que la carrera); desde kGoodSnrDb, bueno.
// kGoodSnrDb y kFullSnrDb (barra llena) son orientativos, sin medida: la demo da 18,5 y 11,2 dB.
inline constexpr double kGoodSnrDb = 18, kFullSnrDb = 30;
enum class Snr { Poor, Fair, Good };
Snr snrLevel(double snrDb);
double snrFill(double snrDb);  // 0..1

// Titulo: el canal (channelLabel, "R4/A6/B3" si se duda; el de HDZero si es digital) o, sin canal, los MHz.
// Detalle: "5769 MHz · NTSC" (sin los MHz si ya son el titulo; el tipo de senal si es digital).
struct Lines {
    QString title, detail;
};
Lines lines(const Pilot &p, const QString &channelLabel);

}  // namespace pilotcard
