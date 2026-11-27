#pragma once
#include <QPalette>
#include <QString>

// Tema de la interfaz: el del sistema (estilo y colores de Windows/KDE, como hasta ahora), claro u oscuro. Los dos
// propios usan el estilo Fusion, su paleta y una hoja de estilo corta: igual en Windows y en Linux. Las graficas
// pintan con la paleta (QPalette::Text, Highlight...), asi que siguen al tema sin tocarlas.
namespace theme {

enum class Theme { System, Light, Dark };

// Ajuste guardado ("theme" en el .ini): "" = sistema, "light", "dark". Lo desconocido, sistema.
Theme parse(const QString &s);
QString name(Theme t);
Theme setting();

// Paleta de un tema propio (Light o Dark). Con System, la del estilo del sistema.
QPalette palette(Theme t);

// Cambia el aspecto de toda la aplicacion al momento y guarda el ajuste.
void apply(Theme t);

}  // namespace theme
