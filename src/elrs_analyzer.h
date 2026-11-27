#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QFontMetrics>
#include <QMap>
#include <QPixmap>
#include <QVector>

#include <algorithm>
#include <optional>

#include "backend.h"
#include "elrs_advisor.h"
#include "survey_session.h"

class QCheckBox;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSpinBox;
class QTextBrowser;
class QTimer;
class QTreeWidget;

// Grafica del analizador de 2,4 GHz: espectro medio y picos (dB sobre el suelo), los canales WiFi detectados (sus
// 16.6 MHz) y, debajo, los 80 canales de ELRS coloreados por lo ocupados que estan. Arriba, la barra de barrido: cada MHz
// se ilumina al volver a medirse (SpectrumPoint::seq) y se va apagando; donde va el barrido, una franja tenue. Con el
// raton encima, su frecuencia.
class ElrsView : public QWidget {
    Q_OBJECT
public:
    explicit ElrsView(QWidget *parent = nullptr);
    void setData(const QVector<SpectrumPoint> &bins, const elrs::Report &r);
    // analisis en marcha: la barra de barrido se anima; al empezar, lo de antes se olvida
    void setScanning(bool on);
    QSize sizeHint() const override { return {760, 300}; }
    static constexpr double kLo = 2398, kHi = 2484;
    static constexpr int kFadeMs = 900;  // lo que tarda un MHz recien medido en apagarse en la barra

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    // ---- paintEvent(), por capas ----
    struct Geometry {  // ejes del pintado: x por MHz (kLo..kHi), y por dB (0..dbMax); fila de WiFi arriba, ELRS abajo
        QFontMetrics fm;
        int left = 0, right = 0, wifiRowH = 0, elrsRowH = 0, barTop = 0, barH = 0, top = 0, plotBottom = 0;
        double dbMax = 40;
        double x(double mhz) const { return left + (mhz - kLo) / (kHi - kLo) * (right - left); }
        double mhz(double px) const { return kLo + (px - left) / (right - left) * (kHi - kLo); }
        double y(double db) const { return plotBottom - std::clamp(db, 0.0, dbMax) / dbMax * (plotBottom - top); }
    };
    void drawGrid(QPainter &p, const Geometry &g) const;
    void drawSweepBar(QPainter &p, const Geometry &g) const;     // que parte se acaba de medir
    void drawUnreachable(QPainter &p, const Geometry &g) const;  // lo que el adaptador no alcanza
    void drawWifi(QPainter &p, const Geometry &g) const;
    void drawSpectrum(QPainter &p, const Geometry &g) const;
    void drawAxis(QPainter &p, const Geometry &g) const;
    void drawChannels(QPainter &p, const Geometry &g) const;     // los 80 canales de ELRS
    void drawCursor(QPainter &p, const Geometry &g) const;       // la frecuencia bajo el raton
    QVector<SpectrumPoint> bins_;
    elrs::Report report_;
    QHash<int, quint32> cellSeq_;  // por MHz: el ultimo seq visto
    QHash<int, qint64> cellT_;     // y cuando cambio (ms de clock_)
    QElapsedTimer clock_;
    QTimer *anim_ = nullptr;
    bool scanning_ = false;
    std::optional<double> mouseX_;  // el raton, si esta sobre la grafica
};

// Analizador de 2,4 GHz (Herramientas): barre 2402-2482 MHz sin parar y dice que canales de ELRS estan ocupados, que
// redes WiFi y otras emisiones hay, y como configurar ELRS para aguantarlo (src/elrs_advisor.h). No se ve video
// mientras esta en marcha. Usa el mismo barrido que el analizador de canales de 5,8 GHz: no pueden ir a la vez.
class ElrsAnalyzer : public QDialog {
    Q_OBJECT
public:
    ElrsAnalyzer(Backend *backend, QWidget *parent = nullptr);
    bool isRunning() const { return session_.running(); }
    void setAdapterOpen(bool open);  // tambien llega solo, con el estado del backend
    // el otro analizador se ha puesto en marcha (el barrido ya es suyo): parar sin pedirselo al backend
    void abandon(const QString &why);
    // el reloj del analisis (s; < 0: el real); lo demas, por sus controles (test/analyzer_driver.h)
    void setTimeForTest(double s) { session_.setTimeForTest(s); }

signals:
    void requestStart(double loMHz, double hiMHz);
    void requestMemory(int sweeps);
    void requestStop();
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    // ---- el constructor, por bloques de la ventana ----
    QHBoxLayout *buildControls();
    QWidget *buildGraph();
    int addLegendRow(QGridLayout *legend, int row, const QVector<QPixmap> &swatches);
    QWidget *buildBottom();
    void connectSettings();
    void retranslate();
    void toggle();
    void stop();
    void onSurvey(const SurveyData &d);
    void onStopped();
    void recompute();
    void updateDetected();
    void holdReport(double t);
    void setDetectedRows(const QStringList &keys, double t);
    void showDetectedTexts(const QStringList &keys, double t);
    void updateTips();
    QString tipText(elrs::Tip t) const;
    QString rateAdvice(elrs::Verdict v) const;
    QString modeTable() const;  // perdida estimada de los modos del uso elegido
    QString verdictText() const;
    void showStopped(const QString &state);  // ya parado: boton y estado
    // El veredicto que se muestra: cambia solo si el nuevo dura kHoldS (no salta a cada barrido). immediate: ya
    // (al cambiar un ajuste, o el primero)
    void settleVerdict(bool immediate);
    void buildLegend();
    void updateAutoStopTip();  // la ayuda de "parar solo" (dice cuantos barridos de media)
    void checkSteady();  // parar solo (autoStop_) cuando lo que se ve lleva kSteadyS sin cambiar
    void noteChange(const QString &from, const QString &to, double t);  // claves de updateTips
    void stopIfDone(double t);
    elrs::Use use() const;
    elrs::Region region() const;

    QComboBox *use_ = nullptr, *region_ = nullptr, *memory_ = nullptr;
    QSpinBox *pilots_ = nullptr;  // otros pilotos con ELRS de 2,4 GHz a la vez
    QLabel *useLbl_ = nullptr, *regionLbl_ = nullptr, *memoryLbl_ = nullptr, *pilotsLbl_ = nullptr;
    QPushButton *startBtn_ = nullptr;
    QCheckBox *autoStop_ = nullptr;  // parar solo cuando el resultado ya no cambia
    QLabel *state_ = nullptr, *summary_ = nullptr, *detectedTitle_ = nullptr, *tipsTitle_ = nullptr;
    // leyenda: muestras de color con su texto (dos filas) y una nota debajo
    QVector<QLabel *> legendText_;
    QLabel *legendGraph_ = nullptr, *legendElrs_ = nullptr, *legendNote_ = nullptr;
    ElrsView *view_ = nullptr;
    QTreeWidget *detected_ = nullptr;
    QTextBrowser *tips_ = nullptr;
    SurveySession session_;  // en marcha, adaptador, paradas pendientes y el reloj del analisis
    // ---- lo que se muestra no salta a cada barrido ----
    static constexpr double kHoldS = 8;      // un veredicto nuevo tiene que durar esto para mostrarse
    static constexpr double kKeepS = 12;     // una sugerencia o algo "en el aire" sigue a la vista esto tras dejar de verse
    static constexpr double kRenderS = 5;    // las sugerencias (sus cifras) se rehacen como mucho cada esto
    static constexpr double kDimS = 5;       // una fila de "En el aire" sin verse esto: mas tenue
    static constexpr double kMinRunS = 30;   // parar solo: al menos esto midiendo (un AP callado se pilla a ratos)
    static constexpr double kSteadyS = 15;   // y el veredicto y las sugerencias no han cambiado en esto
    static constexpr double kMaxRunS = 120;  // y, cambie o no, como mucho esto
    struct Held {                            // una fila de "En el aire"
        QStringList cols;
        QColor colour;
        double seen = 0;
    };
    // Lo de un analisis: al empezar otro se vuelve a los valores de aqui (run_ = Run{}), sin olvidar ninguno
    struct Run {
        SurveyData last;
        elrs::Report report;
        elrs::Report shownReport;            // el informe de las cifras del resumen y la tira (= veredicto mostrado)
        int shownSweep = -1;                 // el barrido de shownReport
        QString lastChange;                  // lo ultimo que cambio (para decirlo al parar a los kMaxRunS)
        QString currentTipsKey;              // la estructura de las sugerencias de ahora (updateTips)
        QString steadyKey;                   // lo que se ve (veredicto, sugerencias, en el aire) y desde cuando
        double steadySince = 0;
        elrs::Verdict shownVerdict = elrs::Verdict::Unknown, candVerdict = elrs::Verdict::Unknown;
        double candSince = 0;
        QHash<int, double> tipSeen;          // por sugerencia: la ultima vez que salio
        QMap<QString, Held> held;            // por clave ("wifi:06", "other:1:2450"): ordenadas
        QStringList shownKeys;
        QString tipsKey, tipsHtml;           // lo ultimo dibujado en las sugerencias (y su estructura)
        double tipsT = -1e9;
        int sweeps = 0;
    };
    Run run_;
};
