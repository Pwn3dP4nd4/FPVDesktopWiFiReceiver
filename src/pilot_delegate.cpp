#include "pilot_delegate.h"
#include "pilot_card.h"

#include <QPainter>
#include <QPainterPath>

namespace {
constexpr int kGap = 3, kPad = 8, kBar = 4;  // entre tarjetas, dentro de la tarjeta, alto de la barra

QColor snrColour(double snrDb) {
    switch (pilotcard::snrLevel(snrDb)) {
    case pilotcard::Snr::Good: return kGoodColour;
    case pilotcard::Snr::Fair: return kWarnColour;
    case pilotcard::Snr::Poor: break;
    }
    return kBadColour;
}

// fondo y borde: el seleccionado, con el color de seleccion; al pasar el raton, el borde algo mas marcado
void drawCard(QPainter *p, const QRectF &card, const QStyleOptionViewItem &opt) {
    const QPalette &pal = opt.palette;
    const bool selected = opt.state & QStyle::State_Selected, hover = opt.state & QStyle::State_MouseOver;
    QPainterPath path;
    path.addRoundedRect(card, 6, 6);
    p->fillPath(path, selected ? withAlpha(pal.color(QPalette::Highlight), 50) : pal.color(QPalette::AlternateBase));
    p->setPen(QPen(selected || hover ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), selected ? 1.5 : 1));
    p->drawPath(path);
}

// barra de SNR: fondo apagado y la parte llena del color de su nivel
void drawSnrBar(QPainter *p, const QRectF &bar, double snrDb, const QPalette &pal) {
    p->setPen(Qt::NoPen);
    p->setBrush(withAlpha(pal.color(QPalette::Text), 30));
    p->drawRoundedRect(bar, kBar / 2.0, kBar / 2.0);
    QRectF fill = bar;
    fill.setWidth(bar.width() * pilotcard::snrFill(snrDb));
    p->setBrush(snrColour(snrDb));
    p->drawRoundedRect(fill, kBar / 2.0, kBar / 2.0);
}
}  // namespace

void PilotDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = opt.palette;
    const QRectF card = QRectF(opt.rect).adjusted(kGap, kGap, -kGap, -kGap);
    drawCard(p, card, opt);

    const bool digital = index.data(DigitalRole).toBool();
    const double snr = index.data(SnrRole).toDouble();
    const QRectF in = card.adjusted(kPad, kPad - 2, -kPad, -kPad);
    QFont title = opt.font;
    title.setBold(true);
    title.setItalic(digital);
    const QFontMetrics tm(title), dm(opt.font);
    const QString snrText = QStringLiteral("%1 dB").arg(snr, 0, 'f', 1);
    QRectF line(in.left(), in.top(), in.width(), tm.height());
    p->setFont(title);
    p->setPen(digital ? pal.color(QPalette::PlaceholderText) : snrColour(snr));
    p->drawText(line, Qt::AlignRight | Qt::AlignVCenter, snrText);
    p->setPen(pal.color(QPalette::Text));
    const QRectF titleRect = line.adjusted(0, 0, -tm.horizontalAdvance(snrText) - kPad, 0);
    p->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                tm.elidedText(index.data(TitleRole).toString(), Qt::ElideRight, int(titleRect.width())));

    line.translate(0, tm.height());
    line.setHeight(dm.height());
    p->setFont(opt.font);
    p->setPen(pal.color(QPalette::PlaceholderText));
    p->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                dm.elidedText(index.data(DetailRole).toString(), Qt::ElideRight, int(line.width())));

    // digital: no hay video analogico que juzgar con kLowSnrDb y kGoodSnrDb; sin barra ni color
    if (!digital) drawSnrBar(p, QRectF(in.left(), in.bottom() - kBar, in.width(), kBar), snr, pal);
    p->restore();
}

QSize PilotDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const {
    QFont title = opt.font;
    title.setBold(true);
    const int h = QFontMetrics(title).height() + QFontMetrics(opt.font).height() + kBar + 4 + 2 * kPad + 2 * kGap;
    return {opt.rect.width(), h};
}
