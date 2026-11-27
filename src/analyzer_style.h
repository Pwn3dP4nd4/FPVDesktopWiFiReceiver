#pragma once
#include <QColor>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

// Lo que comparten la ventana del analizador de 5,8 GHz (src/channel_analyzer.cpp) y su historial de carrera
// (src/race_history.cpp): colores por sistema, la chincheta de un canal fijado y cuando un video es flojo. El cursor
// de frecuencia, tambien en la grafica de 2,4 GHz (src/elrs_analyzer.cpp).

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

// cursor de frecuencia (el raton sobre la grafica): linea punteada en x, de top a bottom, y los MHz en una etiqueta
// arriba, a su derecha (a su izquierda si no cabe antes de right). Se pinta lo ultimo: tapa lo que haya debajo
inline void drawFreqCursor(QPainter &p, const QPalette &pal, double x, double top, double bottom, double right,
                           double mhz) {
    p.save();
    QColor line = pal.color(QPalette::Text);
    line.setAlpha(170);
    p.setPen(QPen(line, 1, Qt::DotLine));
    p.drawLine(QPointF(x, top), QPointF(x, bottom));
    const QString text = QStringLiteral("%1 MHz").arg(mhz, 0, 'f', 0);
    const QFontMetrics fm(p.font());
    const double w = fm.horizontalAdvance(text) + 8, h = fm.height() + 2;
    const QRectF box(x + 4 + w <= right ? x + 4 : x - 4 - w, top, w, h);
    QColor back = pal.color(QPalette::Base);
    back.setAlpha(230);
    line.setAlpha(110);
    p.setPen(QPen(line, 1));
    p.setBrush(back);
    p.drawRoundedRect(box, 3, 3);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(box, Qt::AlignCenter, text);
    p.restore();
}
