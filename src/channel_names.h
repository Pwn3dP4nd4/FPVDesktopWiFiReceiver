#pragma once
// Nombre de canal de un piloto. Varias bandas tienen canales a 0-4 MHz unos de otros (R5 5806 / A4 5805 / B5 5809,
// R7 = F8 5880...) y la portadora medida se desvia 1-3 MHz (cristales del VTX y del adaptador, el centro de la FM se
// mueve con la imagen): con solo la frecuencia no se sabe cual es. Se muestran todos los posibles ("R5/A4/F4") y el
// usuario dice cual es el suyo (se recuerda en el .ini, para siempre, hasta que elija otro de esos).
#include <QString>
#include <QStringList>

struct Pilot;

namespace chnames {

constexpr double kWindowMHz = 4;  // canales estandar a esta distancia de la portadora: todos posibles

struct Resolved {
    QString name;               // el que se usa (OSD, ficheros, analizador); vacio si no hay ninguno cerca
    QStringList alternatives;   // todos los posibles, primero el elegido; vacia si solo hay uno
    bool chosen = false;        // el usuario lo eligio entre las alternativas
    bool ambiguous() const { return alternatives.size() > 1 && !chosen; }
    QString label() const { return ambiguous() ? alternatives.join(QLatin1Char('/')) : name; }
};

// Analogico: los canales a kWindowMHz, Raceband primero (luego F, A, B, E, L) salvo que el usuario haya elegido
// uno de ellos. Digital (HDZero): solo los canales que usa HDZero (R, L, E1, F1, F2, F4), a 3 MHz; no hay dudas.
Resolved resolve(double mhz, bool digital = false);
// Pone name/altNames/nameChosen del piloto segun resolve()
void apply(Pilot &p);
// El usuario dice que su canal es este: se prefiere a los otros posibles cerca de el (y se olvida otra eleccion
// entre ellos). Nombre de kp::fpv_channels() ("F4"); uno que no exista se ignora.
void choose(const QString &name);
// Frecuencia nominal de un canal estandar, 0 si no existe
double channelMHz(const QString &name);

}  // namespace chnames
