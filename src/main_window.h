#pragma once
#include "backend.h"
#include <QMainWindow>

class QListWidget;
class QPushButton;
class QComboBox;
class QLabel;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Backend *backend, QWidget *parent = nullptr);
    void setRate(int rateSel);  // 1 = 40 MS/s, 2 = 20 MS/s

signals:
    void requestOpen();
    void requestClose();
    void requestScan();
    void requestWatch(double freqMHz);
    void requestStop();
    void requestWatchRate(int rateSel);
    void requestGain(double db);
    void requestRecord(double seconds, const QString &prefix);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void onOpened(bool ok, const QString &err);
    void onPilots(const QVector<Pilot> &p);
    void onStatus(const RadioStatus &s);
    void toggleFullScreen();
    void watchChannelFromCombo();

    Backend *backend_;
    QWidget *side_;
    QPushButton *connectBtn_, *scanBtn_, *stopBtn_, *recBtn_;
    QComboBox *channels_, *rate_, *gain_;
    QListWidget *list_;
    QLabel *info_;
    VideoWidget *video_;
    bool open_ = false, opening_ = false;
    double watching_ = 0;
};
