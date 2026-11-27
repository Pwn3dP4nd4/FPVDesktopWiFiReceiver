#include "icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace icons {

namespace {
// Cada icono, en una rejilla de 24 x 24 (se escala al tamano pedido), con trazo de 2.
void plug(QPainter &p) {  // conector: cuerpo, dos patas y el cable
    p.drawRoundedRect(QRectF(7, 8, 10, 8), 2, 2);
    p.drawLine(QPointF(10, 4), QPointF(10, 8));
    p.drawLine(QPointF(14, 4), QPointF(14, 8));
    p.drawLine(QPointF(12, 16), QPointF(12, 21));
}

void scan(QPainter &p) {  // radar: dos arcos, el centro y el haz
    p.drawArc(QRectF(3, 3, 18, 18), 30 * 16, 300 * 16);
    p.drawArc(QRectF(8, 8, 8, 8), 30 * 16, 300 * 16);
    p.drawLine(QPointF(12, 12), QPointF(20, 7));
    p.setBrush(p.pen().color());
    p.drawEllipse(QPointF(12, 12), 1.2, 1.2);
}

void stop(QPainter &p) {
    p.setBrush(p.pen().color());
    p.drawRoundedRect(QRectF(7.5, 7.5, 9, 9), 1, 1);
}

void camera(QPainter &p) {  // cuerpo, visor y objetivo
    QPainterPath body;
    body.addRoundedRect(QRectF(3, 7, 18, 13), 3, 3);
    p.drawPath(body);
    p.drawLine(QPointF(9, 7), QPointF(10.5, 4.5));
    p.drawLine(QPointF(10.5, 4.5), QPointF(13.5, 4.5));
    p.drawLine(QPointF(13.5, 4.5), QPointF(15, 7));
    p.drawEllipse(QPointF(12, 13.5), 3.5, 3.5);
}

void record(QPainter &p) {
    p.setBrush(p.pen().color());
    p.drawEllipse(QPointF(12, 12), 6, 6);
}

void wave(QPainter &p) {  // senal en bruto (IQ): una onda
    QPainterPath w;
    w.moveTo(2, 12);
    w.cubicTo(5, 2, 8, 2, 10, 12);
    w.cubicTo(12, 22, 15, 22, 17, 12);
    w.cubicTo(18.5, 6, 20.5, 6, 22, 12);
    p.drawPath(w);
}

QPixmap draw(Kind k, const QColor &colour, int size) {
    QPixmap px(size, size);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    p.setPen(QPen(colour, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (k) {
    case Kind::Plug: plug(p); break;
    case Kind::Scan: scan(p); break;
    case Kind::Stop: stop(p); break;
    case Kind::Camera: camera(p); break;
    case Kind::Record: record(p); break;
    case Kind::Wave: wave(p); break;
    }
    return px;
}
}  // namespace

QIcon make(Kind k, const QColor &colour) {
    QIcon icon;
    for (int s : {16, 20, 24, 32, 48}) icon.addPixmap(draw(k, colour, s));
    return icon;
}

}  // namespace icons
