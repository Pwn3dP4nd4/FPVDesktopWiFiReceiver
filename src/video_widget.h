#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QWidget>

class QTimer;

// Pinta el ultimo fotograma escalado manteniendo 4:3 (720x480 NTSC / 720x576 PAL son pixeles no cuadrados)
// y un OSD opcional con canal / SNR / estado.
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr);
    void setOsd(const QString &text) { osd_ = text; update(); }
    void setOsdVisible(bool on) { osdOn_ = on; update(); }
    bool osdVisible() const { return osdOn_; }
    void setPlaceholder(const QString &text) { placeholder_ = text; update(); }
    void setLogo(const QPixmap &logo) { logo_ = logo; update(); }
    void clear();

public slots:
    void setFrame(const QImage &f);

signals:
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private:
    QImage frame_;
    QString osd_, placeholder_;
    QPixmap logo_;
    bool osdOn_ = true;
    // Senal perdida: el decodificador deja de emitir fotogramas y el ultimo se quedaria congelado como si
    // fuera en directo. Pasado kStaleMs sin fotograma nuevo se oscurece y se avisa.
    static constexpr int kStaleMs = 400;
    QElapsedTimer sinceFrame_;
    QTimer *staleCheck_ = nullptr;
    bool stale_ = false;
};
