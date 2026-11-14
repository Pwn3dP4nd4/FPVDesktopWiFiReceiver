#pragma once
#include <QImage>
#include <QWidget>

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
    void clear() { frame_ = QImage(); update(); }

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
    bool osdOn_ = true;
};
