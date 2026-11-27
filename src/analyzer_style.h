#pragma once
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

// Lo que comparten la ventana del analizador de 5,8 GHz (src/channel_analyzer.cpp) y su historial de carrera
// (src/race_history.cpp): colores por sistema, la chincheta de un canal fijado y cuando un video es flojo.

// colores de los recomendados (franja) por sistema
inline const QColor kAnalogColour(0x2f, 0x80, 0xed), kHdzColour(0x7b, 0x5c, 0xd6);
// seguimiento de carrera: por debajo de esta SNR (dB) el video de un piloto se tiene por flojo
inline constexpr float kLowSnrDb = 12;

// chincheta de un canal fijado: cabeza redonda y punta en at
inline void drawPin(QPainter &p, QPointF at, const QColor &c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(c, 1.5));
    p.drawLine(at, at + QPointF(0, -5));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(at + QPointF(0, -7.5), 3, 3);
    p.restore();
}

inline QIcon pinIcon(const QColor &c) {
    QPixmap px(12, 12);
    px.fill(Qt::transparent);
    QPainter p(&px);
    drawPin(p, QPointF(6, 11.5), c);
    return QIcon(px);
}
