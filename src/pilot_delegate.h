#pragma once
#include <QStyledItemDelegate>

// Pinta cada piloto de la lista de la ventana principal como una tarjeta: canal y SNR (de color) arriba, MHz y
// norma debajo y una barra con la SNR. Lo que dice cada una: pilotcard:: (src/pilot_card.h). El texto de la fila
// (Qt::DisplayRole) sigue siendo el de una linea, para copiar y para los lectores de pantalla.
class PilotDelegate : public QStyledItemDelegate {
public:
    // datos de la fila; Qt::UserRole son los MHz (los usa la ventana para ver el canal)
    enum Role { TitleRole = Qt::UserRole + 1, DetailRole, SnrRole, DigitalRole };

    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override;
};
