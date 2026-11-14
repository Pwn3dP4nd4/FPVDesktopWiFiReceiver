#include "video_widget.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent) {
    setMinimumSize(360, 270);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    staleCheck_ = new QTimer(this);
    staleCheck_->setInterval(200);
    connect(staleCheck_, &QTimer::timeout, this, [this] {
        if (frame_.isNull()) return;
        if (sinceFrame_.elapsed() > kStaleMs) {
            stale_ = true;
            update();  // tambien refresca el contador de segundos
        }
    });
}

void VideoWidget::setFrame(const QImage &f) {
    frame_ = f;
    sinceFrame_.start();
    stale_ = false;
    if (!staleCheck_->isActive()) staleCheck_->start();
    update();
}

void VideoWidget::clear() {
    frame_ = QImage();
    stale_ = false;
    staleCheck_->stop();
    update();
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *) { emit doubleClicked(); }

void VideoWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (frame_.isNull()) {
        QRect textRect = rect().adjusted(20, 20, -20, -20);
        if (!logo_.isNull() && height() > 300) {
            const QSize ls = logo_.size() / logo_.devicePixelRatio();
            const QRect lr(QPoint((width() - ls.width()) / 2, height() / 2 - ls.height() - 10), ls);
            p.setOpacity(0.9);
            p.drawPixmap(lr, logo_);
            p.setOpacity(1.0);
            textRect.setTop(lr.bottom() + 20);
        }
        if (!placeholder_.isEmpty()) {
            p.setPen(QColor(180, 180, 180));
            p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, placeholder_);
        }
    } else {
        // Analogico: 4:3 sea cual sea la resolucion del fotograma
        QSize s(4, 3);
        s.scale(size(), Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), s);
        r.moveCenter(rect().center());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, frame_);
        if (stale_) {
            p.fillRect(r, QColor(0, 0, 0, 170));
            QFont big = font();
            big.setBold(true);
            big.setPixelSize(qMax(18, r.height() / 9));
            p.setFont(big);
            p.setPen(QColor(255, 80, 60));
            const QFontMetrics bm(big);
            QRect nr = r;
            nr.setBottom(r.center().y() + bm.height() / 2);
            p.drawText(nr, Qt::AlignHCenter | Qt::AlignBottom, tr("NO SIGNAL"));
            QFont small = font();
            small.setPixelSize(qMax(12, r.height() / 30));
            p.setFont(small);
            p.setPen(QColor(220, 220, 220));
            QRect sr = r;
            sr.setTop(nr.bottom() + 8);
            p.drawText(sr, Qt::AlignHCenter | Qt::AlignTop,
                       tr("Last picture %1 s ago").arg(sinceFrame_.elapsed() / 1000.0, 0, 'f', 1));
        }
    }
    if (osdOn_ && !osd_.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.1);
        f.setBold(true);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QRect tr = fm.boundingRect(osd_).adjusted(-10, -5, 10, 5).translated(14 + 10, 14 + fm.ascent() + 5);
        QPainterPath path;
        path.addRoundedRect(tr, tr.height() / 2.0, tr.height() / 2.0);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(path, QColor(0, 0, 0, 150));
        p.setPen(Qt::white);
        p.drawText(tr, Qt::AlignCenter, osd_);
    }
}
