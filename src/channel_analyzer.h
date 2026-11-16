#pragma once
#include <QDialog>
#include <QVector>

#include "backend.h"
#include "channel_advisor.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;

// Grafica del analizador: espectro medio y picos (dB sobre el suelo), zonas con rafagas, los canales candidatos
// coloreados por interferencia, lo que ya hay en el aire y los canales recomendados.
class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget *parent = nullptr);
    void setData(const SurveyData &d, const advisor::Advice &a, double loMHz, double hiMHz);
    QSize sizeHint() const override { return {760, 300}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    SurveyData data_;
    advisor::Advice advice_;
    double lo_ = 5640, hi_ = 5950;
};

// Analizador de canales (Herramientas): barre las bandas elegidas sin parar y recomienda canales para N pilotos
// analogicos y M HDZero (src/channel_advisor.h). No se ve video mientras esta en marcha.
class ChannelAnalyzer : public QDialog {
    Q_OBJECT
public:
    ChannelAnalyzer(Backend *backend, QWidget *parent = nullptr);
    void setAdapterOpen(bool open);
    // test/analyzer_shot.cpp: pilotos (< 0 = los guardados) y empezar sin clic
    void setCounts(int analog, int hdzero);
    void startForTest() { if (!running_) toggle(); }

signals:
    void requestStart(double loMHz, double hiMHz);
    void requestStop();
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void retranslate();
    void toggle();
    void stop();
    void onSurvey(const SurveyData &d);
    void recompute();
    QString bands() const;
    void range(double &lo, double &hi) const;
    QString noteText(const advisor::Pick &p) const;

    QVector<QCheckBox *> bandBoxes_;
    QSpinBox *nAnalog_ = nullptr, *nHdz_ = nullptr;
    QPushButton *startBtn_ = nullptr;
    QLabel *bandsTitle_ = nullptr, *pilotsTitle_ = nullptr, *analogLbl_ = nullptr, *hdzLbl_ = nullptr;
    QLabel *hint_ = nullptr, *state_ = nullptr, *summary_ = nullptr, *legend_ = nullptr;
    SpectrumView *view_ = nullptr;
    QTreeWidget *table_ = nullptr;
    SurveyData last_;
    bool running_ = false, open_ = false;
};
