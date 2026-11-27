#pragma once
#include <QString>

#include "backend.h"

// Textos del estado de la radio que muestra la ventana principal, sin widgets: el OSD sobre el video y el recuadro
// de informacion del panel lateral. Se traducen en el contexto "MainWindow" (de donde salieron: el .ts no cambia).
namespace statustext {

// "HDZero", "HDZero narrow", "DJI 20 MHz", "digital 18 MHz"
QString digitalKind(const Pilot &p);
// "Scanning 3/10" o "R5/A4  5806 MHz  SNR 19 dB  PAL"; digital: la senal digital que se esta viendo (sin imagen)
QString osd(const RadioStatus &s, const Pilot *digital);
// el stream con el modo desarrollador; los errores y el aviso de USB 2, siempre
QString info(const RadioStatus &s, bool devMode);

}  // namespace statustext
