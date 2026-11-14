#pragma once
#include "backend.h"
#include <QMainWindow>

class QAction;
class QListWidget;
class QPushButton;
class QComboBox;
class QLabel;
class QSettings;
class VideoWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Backend *backend, QWidget *parent = nullptr);
    ~MainWindow() override;
    void setRate(int rateSel);  // 1 = 40 MS/s, 2 = 20 MS/s (no se guarda: opcion de linea de comandos)

    // Arranque: conecta si el ajuste "Conectar el adaptador al abrir" esta activo (por defecto) o si se pidio
    // por linea de comandos. Tras conectar: watchMHz > 0 -> ese canal; si no, el ultimo canal visto o un escaneo.
    void startup(bool autoConnect, double watchMHz, bool forceScan);
    static QSettings &settings();

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
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    void connectAdapter(bool userInitiated);
    void onOpened(bool ok, const QString &err);
    void onPilots(const QVector<Pilot> &p);
    void onStatus(const RadioStatus &s);
    void toggleFullScreen();
    void watchChannelFromCombo();
    void showAbout();
    QString recordingsDir() const;

    Backend *backend_;
    QWidget *side_;
    QPushButton *connectBtn_, *scanBtn_, *stopBtn_, *recBtn_;
    QComboBox *channels_, *rate_, *gain_;
    QListWidget *list_;
    QLabel *info_;
    VideoWidget *video_;
    QAction *autoConnectAct_ = nullptr, *resumeLastAct_ = nullptr;
    bool open_ = false, opening_ = false, userConnect_ = false;
    double watching_ = 0, lastSaved_ = 0;
    double pendingWatch_ = 0;   // canal a ver al terminar de conectar
    bool pendingScan_ = false;  // escanear al terminar de conectar
};
