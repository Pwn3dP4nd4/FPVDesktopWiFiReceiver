#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QWidget>

class QTimer;

// Pinta el ultimo fotograma escalado a la forma elegida (4:3 por defecto: 720x480 NTSC / 720x576 PAL son
// pixeles no cuadrados), con recorte opcional del sobrebarrido, y un OSD opcional con canal / SNR / estado.
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
    enum class Aspect { R4x3, R16x9, Fill };  // Fill: estirar a todo el widget
    void setAspect(Aspect a) { aspect_ = a; update(); }
    void setCropOverscan(bool on) { crop_ = on; update(); }

public slots:
    // producedNs: reloj monotono (perf_now_ns()) cuando el backend tuvo el fotograma; 0 = desconocido
    void setFrame(const QImage &f, qint64 producedNs = 0);

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
    static constexpr double kCropX = 0.03, kCropY = 0.03;  // por lado: ~6 % de cada eje, como un TV
    QElapsedTimer sinceFrame_;
    QTimer *staleCheck_ = nullptr;
    bool stale_ = false;
    Aspect aspect_ = Aspect::R4x3;
    bool crop_ = true;

    // Medidas (log cada 5 s con --verbose): coste de pintar y latencia backend -> pantalla pintada
    void perfPainted(qint64 paintNs);
    qint64 pendingProducedNs_ = 0;  // del fotograma aun no pintado
    struct Perf {
        int frames = 0, painted = 0, replaced = 0;  // llegados / pintados / sustituidos antes de pintarse
        qint64 paintSum = 0, paintMax = 0, latSum = 0, latMax = 0;
        int latN = 0;
    } perf_;
    QElapsedTimer perfT_;
};

// Reloj monotono en ns comun a backend y GUI (para la latencia).
qint64 perf_now_ns();
