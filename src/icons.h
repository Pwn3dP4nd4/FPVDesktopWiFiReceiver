#pragma once
#include <QColor>
#include <QIcon>

// Iconos de linea de los botones de la ventana principal, dibujados con QPainter en el color que se pida (el del
// texto del boton): siguen al tema sin depender de Qt SVG ni de los iconos del estilo (los de Fusion para
// reproducir son negros y no se ven en el tema oscuro).
namespace icons {

enum class Kind { Plug, Scan, Stop, Camera, Record, Wave };

QIcon make(Kind k, const QColor &colour);

}  // namespace icons
