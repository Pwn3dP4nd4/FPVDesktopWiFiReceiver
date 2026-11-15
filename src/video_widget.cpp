#include "video_widget.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <chrono>

#include "backend.h"  // Backend::gatedFrames
#include "log.h"

qint64 perf_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

void VideoWidget::setFrame(const QImage &f, qint64 producedNs) {
    if (pendingProducedNs_) perf_.replaced++;  // el anterior no llego a pintarse
    pendingProducedNs_ = producedNs ? producedNs : -1;
    perf_.frames++;
    frame_ = f;
    sinceFrame_.start();
    stale_ = false;
    if (!staleCheck_->isActive()) staleCheck_->start();
    update();
}

void VideoWidget::clear() {
    frame_ = QImage();
    pendingProducedNs_ = 0;
    stale_ = false;
    staleCheck_->stop();
    update();
}

QRectF VideoWidget::sourceRect() const {
    QRectF src(frame_.rect());
    if (crop_) src.adjust(src.width() * kCropX, src.height() * kCropY, -src.width() * kCropX, -src.height() * kCropY);
    return src;
}

QImage VideoWidget::snapshot() const {
    if (frame_.isNull()) return {};
    const QRectF src = sourceRect();
    QSize out(qRound(src.width()), qRound(src.height()));
    if (aspect_ != Aspect::Fill) {
        const double a = aspect_ == Aspect::R16x9 ? 16.0 / 9 : 4.0 / 3;
        if (out.width() < out.height() * a) out.setWidth(qRound(out.height() * a));
        else out.setHeight(qRound(out.width() / a));
    }
    QImage img(out, QImage::Format_RGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRectF(img.rect()), frame_, src);
    p.end();
    return img;
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *) { emit doubleClicked(); }

void VideoWidget::paintEvent(QPaintEvent *) {
    const qint64 t0 = perf_now_ns();
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
        // Analogico: 4:3 sea cual sea la resolucion del fotograma; 16:9 para camaras en modo anamorfico
        QRect r = rect();
        if (aspect_ != Aspect::Fill) {
            QSize s = aspect_ == Aspect::R16x9 ? QSize(16, 9) : QSize(4, 3);
            s.scale(size(), Qt::KeepAspectRatio);
            r = QRect(QPoint(0, 0), s);
            r.moveCenter(rect().center());
        }
        // Sobrebarrido: como un televisor, fuera los bordes (inicio/fin de linea, medias lineas arriba/abajo)
        const QRectF src = sourceRect();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(r), frame_, src);
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
    if (!rec_.isEmpty()) {  // grabando: tambien en pantalla completa y sin senal
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.1);
        f.setBold(true);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int dot = fm.height() * 2 / 3;
        QRect box(0, 0, fm.horizontalAdvance(rec_) + dot + 26, fm.height() + 10);
        box.moveTopRight(QPoint(width() - 14, 14));  // a la altura del OSD
        QPainterPath path;
        path.addRoundedRect(box, box.height() / 2.0, box.height() / 2.0);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(path, QColor(0, 0, 0, 150));
        if ((QDateTime::currentMSecsSinceEpoch() / 500) % 2 == 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(235, 40, 40));
            p.drawEllipse(QRect(box.left() + 10, box.center().y() - dot / 2, dot, dot));
        }
        p.setPen(Qt::white);
        p.drawText(box.adjusted(dot + 16, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, rec_);
    }
    p.end();
    if (!frame_.isNull() && pendingProducedNs_) perfPainted(perf_now_ns() - t0);
}

void VideoWidget::perfPainted(qint64 paintNs) {
    const qint64 now = perf_now_ns();
    perf_.painted++;
    perf_.paintSum += paintNs;
    perf_.paintMax = qMax(perf_.paintMax, paintNs);
    if (pendingProducedNs_ > 0) {
        const qint64 lat = now - pendingProducedNs_;
        perf_.latSum += lat;
        perf_.latMax = qMax(perf_.latMax, lat);
        perf_.latN++;
    }
    pendingProducedNs_ = 0;
    if (!perfT_.isValid()) perfT_.start();
    if (perfT_.elapsed() < 5000) return;
    const double secs = perfT_.elapsed() / 1000.0;
    KP_LOGD("perf: %dx%d px, %.0f fps llegados / %.0f pintados (%d sustituidos), pintar media %.2f ms max %.2f ms "
            "(%.0f %% de un nucleo), latencia backend->pintado media %.1f ms max %.1f ms, descartados en el backend %d",
            width(), height(), perf_.frames / secs, perf_.painted / secs, perf_.replaced,
            perf_.paintSum / 1e6 / qMax(1, perf_.painted), perf_.paintMax / 1e6, perf_.paintSum / 1e7 / secs,
            perf_.latSum / 1e6 / qMax(1, perf_.latN), perf_.latMax / 1e6, Backend::gatedFrames.exchange(0));
    perf_ = {};
    perfT_.restart();
}
