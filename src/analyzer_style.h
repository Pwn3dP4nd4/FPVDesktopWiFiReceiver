#pragma once
#include <QColor>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "backend.h"  // SpectrumPoint

// Lo que comparten la ventana del analizador de 5,8 GHz (src/channel_analyzer.cpp) y su historial de carrera
// (src/race_history.cpp): colores por sistema, la chincheta de un canal fijado y cuando un video es flojo. El cursor
// de frecuencia, tambien en la grafica de 2,4 GHz (src/elrs_analyzer.cpp).

// Colores con significado, iguales con cualquier tema (src/theme.h solo cambia fondos y textos). Cada uno, aqui:
// las graficas, las leyendas, las tablas y los avisos que hablan de lo mismo usan el mismo.
// recomendados (franja) y canales fijados, por sistema
inline const QColor kAnalogColour(0x2f, 0x80, 0xed), kHdzColour(0x7b, 0x5c, 0xd6);
// estado de un canal o de un aviso: limpio / tuyo / bien, algo de interferencia / aviso, ocupado / grave
inline const QColor kGoodColour(0x3a, 0xa8, 0x5a), kWarnColour(0xe0, 0xa0, 0x20), kBadColour(0xd9, 0x48, 0x3b);
// cambios aplicados en el historial de carrera
inline const QColor kInfoColour(0x2f, 0x80, 0xed);
// sin dato o de fondo (sin medir, inicio de carrera)
inline const QColor kNeutralColour(0x80, 0x80, 0x80);
// picos (rafaga mas fuerte reciente), rafagas (WiFi: se pinta translucido), WiFi visto (marca)
inline const QColor kPeakColour(0xe0, 0x70, 0x30), kBurstColour(255, 140, 0), kWifiSeenColour(0xc0, 0x60, 0x00);
// la barra de barrido (5,8 y 2,4 GHz), siempre azul: el color de resalte del sistema puede ser cualquiera (gris, verde...)
inline const QColor kSweepColour(0x1e, 0x90, 0xff);

inline QColor withAlpha(QColor c, int alpha) {
    c.setAlpha(alpha);
    return c;
}
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

// texto del cursor de frecuencia: "5806 MHz · R5" (channel: el canal que cae ahi; vacio, ninguno) y, si hay un
// bin medido a menos de medio MHz (el mas cercano), "avg 12 dB · peak 20 dB"
inline QStringList freqCursorText(double mhz, const QString &channel, const QVector<SpectrumPoint> &bins) {
    QStringList t{QStringLiteral("%1 MHz").arg(mhz, 0, 'f', 0) +
                  (channel.isEmpty() ? QString() : QStringLiteral(" \u00b7 ") + channel)};
    const SpectrumPoint *near = nullptr;
    for (const auto &b : bins)
        if (std::abs(b.mhz - mhz) <= 0.5 && (!near || std::abs(b.mhz - mhz) < std::abs(near->mhz - mhz))) near = &b;
    if (near)
        t << QCoreApplication::translate("FreqCursor", "avg %1 dB \u00b7 peak %2 dB")
                 .arg(near->meanDb, 0, 'f', 0)
                 .arg(near->peakDb, 0, 'f', 0);
    return t;
}

// cursor de frecuencia (el raton sobre la grafica): linea punteada en x, de top a bottom, y su texto
// (freqCursorText) en una etiqueta arriba, a su derecha (a su izquierda si no cabe antes de right). Se pinta lo
// ultimo: tapa lo que haya debajo
inline void drawFreqCursor(QPainter &p, const QPalette &pal, double x, double top, double bottom, double right,
                           const QStringList &text) {
    p.save();
    QColor line = pal.color(QPalette::Text);
    line.setAlpha(170);
    p.setPen(QPen(line, 1, Qt::DotLine));
    p.drawLine(QPointF(x, top), QPointF(x, bottom));
    const QFontMetrics fm(p.font());
    double w = 0;
    for (const QString &l : text) w = std::max(w, double(fm.horizontalAdvance(l)));
    w += 8;
    const double h = fm.height() * text.size() + 2;
    const QRectF box(x + 4 + w <= right ? x + 4 : x - 4 - w, top, w, h);
    QColor back = pal.color(QPalette::Base);
    back.setAlpha(230);
    line.setAlpha(110);
    p.setPen(QPen(line, 1));
    p.setBrush(back);
    p.drawRoundedRect(box, 3, 3);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(box.adjusted(4, 1, -4, -1), Qt::AlignLeft | Qt::AlignVCenter, text.join(QLatin1Char('\n')));
    p.restore();
}
