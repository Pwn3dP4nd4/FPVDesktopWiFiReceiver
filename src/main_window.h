#pragma once
#include "backend.h"
#include <QElapsedTimer>
#include <QMainWindow>

class QAction;
class QActionGroup;
class QListWidget;
class QMenu;
class QPushButton;
class QComboBox;
class QLabel;
class QSettings;
class QTimer;
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
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    enum class Screen { Idle, Connecting, Ready, Error, Lost };
    void buildMenus();
    void setupShortcuts();  // F / F11 / Esc / O
    void retranslate();  // todos los textos visibles; se llama al crear y al cambiar de idioma
    void updatePlaceholder();
    void updateConnectButton();
    void connectAdapter(bool userInitiated);
    void onOpened(bool ok, const QString &err);
    void onLost(const QString &msg);
    void onPilots(const QVector<Pilot> &p);
    void onStatus(const RadioStatus &s);
    void toggleFullScreen();
    void watchChannelFromCombo();
    void showAbout();
#ifdef _WIN32
    void installDriver();  // lanza scripts/windows-driver.ps1 como administrador
#endif
    QString recordingsDir() const;
    void updateRecordButton();
    void startRecording();
    void checkRecording();

    Backend *backend_;
    QWidget *side_;
    QPushButton *connectBtn_, *scanBtn_, *stopBtn_, *recBtn_;
    QComboBox *channels_, *rate_, *gain_;
    QListWidget *list_;
    QLabel *info_, *pilotsLabel_, *rateLabel_, *gainLabel_;
    VideoWidget *video_;
    QMenu *fileMenu_ = nullptr, *optMenu_ = nullptr, *langMenu_ = nullptr, *helpMenu_ = nullptr;
    QAction *recDirAct_ = nullptr, *logDirAct_ = nullptr, *shortcutAct_ = nullptr, *driverAct_ = nullptr, *quitAct_ = nullptr;
    QAction *autoConnectAct_ = nullptr, *onConnectSection_ = nullptr, *resumeLastAct_ = nullptr, *scanAct_ = nullptr;
    QAction *langSystemAct_ = nullptr, *manualAct_ = nullptr, *aboutAct_ = nullptr;
    QActionGroup *langGroup_ = nullptr;
    bool open_ = false, opening_ = false, userConnect_ = false;
    double watching_ = 0, lastSaved_ = 0;
    double pendingWatch_ = 0;   // canal a ver al terminar de conectar
    bool pendingScan_ = false;  // escanear al terminar de conectar
    Screen screen_ = Screen::Idle;
    QString openError_;  // texto para Screen::Error / Screen::Lost
    RadioStatus lastStatus_;
    // grabacion de IQ en curso ("" = ninguna)
    static constexpr int kRecSeconds = 5;
    QString recPrefix_;
    QElapsedTimer recStarted_;
    QTimer *recCheck_ = nullptr;
};
