#include "main_window.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QStorageInfo>
#include <QThreadPool>
#include <QToolButton>
#include <QTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "i18n.h"
#include "scan_channels_dialog.h"
#include "spectrum.h"  // kp::fpv_channels(): la tabla de canales de kepyur
#include "video_recorder.h"
#include "video_widget.h"

namespace {
const char *kAppName = "FPVDesktopWiFiReceiver";
const char *kRepoUrl = "https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver";
#ifdef _WIN32
// WinUSB no sostiene de forma estable las ~24.400 transferencias/s de 40 MS/s (la version web de kepyur
// tambien ve a 20). Misma imagen: el filtro RX deja +-10 MHz en ambos casos. El escaneo sigue a 40 MS/s.
constexpr int kDefaultRateIndex = 1;  // 20 MS/s
#else
constexpr int kDefaultRateIndex = 0;  // 40 MS/s
#endif
}  // namespace

QSettings &MainWindow::settings() {
    // INI en %APPDATA%\FPVDesktopWiFiReceiver (Windows) o ~/.config/FPVDesktopWiFiReceiver (Linux)
    static QSettings s(QSettings::IniFormat, QSettings::UserScope, kAppName, kAppName);
    return s;
}

MainWindow::MainWindow(Backend *backend, QWidget *parent) : QMainWindow(parent), backend_(backend) {
    setWindowTitle(backend->name() == QLatin1String("demo")
                       ? QStringLiteral("%1 %2 [demo]").arg(kAppName, KP_VERSION)
                       : QStringLiteral("%1 %2").arg(kAppName, KP_VERSION));

    auto *central = new QWidget;
    auto *h = new QHBoxLayout(central);
    h->setContentsMargins(6, 6, 6, 6);

    side_ = new QWidget;
    side_->setFixedWidth(260);
    auto *left = new QVBoxLayout(side_);
    left->setContentsMargins(0, 0, 0, 0);

    connectBtn_ = new QPushButton;
    scanBtn_ = new QPushButton;
    stopBtn_ = new QPushButton;
    recBtn_ = new QPushButton;
    snapBtn_ = new QPushButton;
    dvrBtn_ = new QPushButton;
    left->addWidget(connectBtn_);
    auto *scanRow = new QHBoxLayout;
    scanRow->setSpacing(4);
    scanRow->addWidget(scanBtn_, 1);
    scanCfgBtn_ = new QToolButton;
    scanCfgBtn_->setText(QString(QChar(0x2699)));  // engranaje
    scanCfgBtn_->setMinimumWidth(scanBtn_->sizeHint().height());
    scanRow->addWidget(scanCfgBtn_);
    left->addLayout(scanRow);

    pilotsLabel_ = new QLabel;
    left->addWidget(pilotsLabel_);
    list_ = new QListWidget;
    left->addWidget(list_, 1);

    channels_ = new QComboBox;
    channels_->addItem(QString(), 0.0);  // "Canal manual..." (retranslate)
    for (const auto &c : kp::fpv_channels())
        channels_->addItem(QStringLiteral("%1  %2 MHz").arg(c.name).arg(c.mhz, 0, 'f', 0), c.mhz);
    left->addWidget(channels_);

    devBox_ = new QWidget;
    auto *form = new QFormLayout(devBox_);
    form->setContentsMargins(0, 0, 0, 0);
    rate_ = new QComboBox;
    rate_->addItem(QString(), 1);
    rate_->addItem(QStringLiteral("20 MS/s"), 2);
    rate_->setCurrentIndex(qBound(0, settings().value("rateIndex", kDefaultRateIndex).toInt(), 1));
    gain_ = new QComboBox;
    gain_->addItem(QString(), -1.0);  // "AGC"
    for (int db : {10, 20, 30, 40, 50}) gain_->addItem(QStringLiteral("%1 dB").arg(db), double(db));
    gain_->setCurrentIndex(qBound(0, settings().value("gainIndex", 0).toInt(), gain_->count() - 1));
    rateLabel_ = new QLabel;
    gainLabel_ = new QLabel;
    form->addRow(rateLabel_, rate_);
    form->addRow(gainLabel_, gain_);
    left->addWidget(devBox_);

    left->addWidget(stopBtn_);
    auto *mediaRow = new QHBoxLayout;  // captura (PNG) y grabacion de video (MP4)
    mediaRow->setSpacing(4);
    mediaRow->addWidget(snapBtn_, 1);
    mediaRow->addWidget(dvrBtn_, 1);
    left->addLayout(mediaRow);
    left->addWidget(recBtn_);

    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setStyleSheet(QStringLiteral("QLabel{font-family:monospace;font-size:11px;}"));
    left->addWidget(info_);

    video_ = new VideoWidget;
    video_->setLogo(QPixmap(QStringLiteral(":/icons/app-128.png")));
    dvr_ = new VideoRecorder(this);
    dvrTick_ = new QTimer(this);
    dvrTick_->setInterval(250);
    connect(dvrTick_, &QTimer::timeout, this, &MainWindow::updateMediaButtons);
    dvrSpace_ = new QTimer(this);
    dvrSpace_->setInterval(5000);
    connect(dvrSpace_, &QTimer::timeout, this, &MainWindow::checkVideoSpace);
    connect(dvr_, &VideoRecorder::finished, this, [this](const VideoRecorder::Result &r) {
        onVideoRecorded(r.path, r.ok, r.noPicture, r.error, r.frames, r.dropped, r.seconds);
    });
    h->addWidget(side_);
    h->addWidget(video_, 1);
    setCentralWidget(central);
    buildMenus();
    setupShortcuts();

    // GUI -> backend (otro hilo: conexiones en cola)
    connect(this, &MainWindow::requestOpen, backend, &Backend::openDevice);
    connect(this, &MainWindow::requestClose, backend, &Backend::closeDevice);
    connect(this, &MainWindow::requestScan, backend, &Backend::startScan);
    connect(this, &MainWindow::requestScanChannels, backend, &Backend::setScanChannels);
    connect(this, &MainWindow::requestWatch, backend, &Backend::watch);
    connect(this, &MainWindow::requestStop, backend, &Backend::stop);
    connect(this, &MainWindow::requestWatchRate, backend, &Backend::setWatchRate);
    connect(this, &MainWindow::requestGain, backend, &Backend::setGain);
    connect(this, &MainWindow::requestRecord, backend, &Backend::record);
    // backend -> GUI
    connect(backend, &Backend::frameReady, this, [this](const QImage &img, qint64 producedNs) {
        video_->setFrame(img, producedNs);
        backend_->frameConsumed();
    });
    connect(backend, &Backend::opened, this, &MainWindow::onOpened);
    connect(backend, &Backend::lost, this, &MainWindow::onLost);
    connect(backend, &Backend::pilotsChanged, this, &MainWindow::onPilots);
    connect(backend, &Backend::statusChanged, this, &MainWindow::onStatus);
    connect(backend, &Backend::message, this, [this](const QString &s) { statusBar()->showMessage(s, 8000); });

    connect(connectBtn_, &QPushButton::clicked, this, [this] {
        if (opening_) return;
        if (open_) {
            emit requestClose();
            open_ = false;
            video_->clear();
            list_->clear();
            onOpened(false, {});
            return;
        }
        connectAdapter(true);
    });
    connect(scanBtn_, &QPushButton::clicked, this, [this] {
        list_->clear();
        emit requestScan();
    });
    connect(scanCfgBtn_, &QToolButton::clicked, this, &MainWindow::chooseScanChannels);
    // seleccion guardada (sin clave = todos); llega al backend antes de cualquier escaneo (cola del hilo)
    scanChannels_ = settings().value("scanChannels").toStringList();
    if (ScanChannelsDialog::frequencies(scanChannels_).isEmpty()) scanChannels_.clear();
    emit requestScanChannels(ScanChannelsDialog::frequencies(scanChannels_));
    connect(stopBtn_, &QPushButton::clicked, this, [this] {
        emit requestStop();
        video_->clear();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        emit requestWatch(it->data(Qt::UserRole).toDouble());
    });
    connect(channels_, &QComboBox::activated, this, [this] { watchChannelFromCombo(); });
    connect(rate_, &QComboBox::activated, this, [this] {
        settings().setValue("rateIndex", rate_->currentIndex());
        emit requestWatchRate(rate_->currentData().toInt());
        if (watching_ > 0) emit requestWatch(watching_);  // la tasa se aplica en el siguiente watch()
    });
    connect(gain_, &QComboBox::activated, this, [this] {
        settings().setValue("gainIndex", gain_->currentIndex());
        emit requestGain(gain_->currentData().toDouble());
        if (watching_ > 0) emit requestWatch(watching_);
    });
    connect(recBtn_, &QPushButton::clicked, this, &MainWindow::startRecording);
    connect(snapBtn_, &QPushButton::clicked, this, &MainWindow::takeSnapshot);
    connect(dvrBtn_, &QPushButton::clicked, this, &MainWindow::toggleVideoRecording);
    recCheck_ = new QTimer(this);
    recCheck_->setInterval(500);
    connect(recCheck_, &QTimer::timeout, this, &MainWindow::checkRecording);
    connect(video_, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullScreen);

    applyDeveloperMode(devModeAct_->isChecked());
    retranslate();
    onOpened(false, {});
    if (!restoreGeometry(settings().value("geometry").toByteArray())) resize(1100, 660);
}

// La grabacion de video se termina en closeEvent(): main.cpp destruye el backend antes que la ventana.
MainWindow::~MainWindow() { snapPool_.waitForDone(); }  // por si se destruye sin closeEvent()

void MainWindow::buildMenus() {
    fileMenu_ = menuBar()->addMenu(QString());
    // Los atajos S / R van en setupShortcuts() (funcionan tambien en pantalla completa); aqui solo se muestran
    snapAct_ = fileMenu_->addAction(QString(), this, &MainWindow::takeSnapshot);
    dvrAct_ = fileMenu_->addAction(QString(), this, &MainWindow::toggleVideoRecording);
    snapDirMenu_ = fileMenu_->addMenu(QString());
    snapDirOpenAct_ = snapDirMenu_->addAction(QString(), this, [this] {
        QDir().mkpath(snapshotsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(snapshotsDir()));
    });
    snapDirChangeAct_ = snapDirMenu_->addAction(QString(), this, [this] { chooseMediaDir("snapshotDir", false); });
    videoDirMenu_ = fileMenu_->addMenu(QString());
    videoDirOpenAct_ = videoDirMenu_->addAction(QString(), this, [this] {
        QDir().mkpath(videosDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(videosDir()));
    });
    videoDirChangeAct_ = videoDirMenu_->addAction(QString(), this, [this] { chooseMediaDir("videoDir", true); });
    fileMenu_->addSeparator();
    recDirAct_ = fileMenu_->addAction(QString(), this, [this] {
        QDir().mkpath(recordingsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(recordingsDir()));
    });
    logDirAct_ = fileMenu_->addAction(QString(), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)));
    });
#ifdef _WIN32
    shortcutAct_ = fileMenu_->addAction(QString(), this, [this] {
        const QString lnk = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) +
                            QStringLiteral("/%1.lnk").arg(kAppName);
        QFile::remove(lnk);
        if (QFile::link(QApplication::applicationFilePath(), lnk))
            statusBar()->showMessage(tr("Desktop shortcut created"), 6000);
        else
            QMessageBox::warning(this, kAppName, tr("Could not create the shortcut."));
    });
    driverAct_ = fileMenu_->addAction(QString(), this, &MainWindow::installDriver);
#endif
    fileMenu_->addSeparator();
    quitAct_ = fileMenu_->addAction(QString(), QKeySequence::Quit, this, &QWidget::close);

    // Ver: forma de la imagen y recorte de bordes (se guardan)
    viewMenu_ = menuBar()->addMenu(QString());
    aspectSection_ = viewMenu_->addSection(QString());
    auto *aspGrp = new QActionGroup(this);
    for (int i = 0; i < 3; ++i) {
        aspectActs_[i] = viewMenu_->addAction(QString());
        aspectActs_[i]->setCheckable(true);
        aspectActs_[i]->setData(i);
        aspGrp->addAction(aspectActs_[i]);
    }
    const int asp = qBound(0, settings().value("aspect", 0).toInt(), 2);
    aspectActs_[asp]->setChecked(true);
    video_->setAspect(static_cast<VideoWidget::Aspect>(asp));
    connect(aspGrp, &QActionGroup::triggered, this, [this](QAction *a) {
        settings().setValue("aspect", a->data().toInt());
        video_->setAspect(static_cast<VideoWidget::Aspect>(a->data().toInt()));
    });
    viewMenu_->addSeparator();
    cropAct_ = viewMenu_->addAction(QString());
    cropAct_->setCheckable(true);
    cropAct_->setChecked(settings().value("cropOverscan", true).toBool());
    video_->setCropOverscan(cropAct_->isChecked());
    connect(cropAct_, &QAction::toggled, this, [this](bool on) {
        settings().setValue("cropOverscan", on);
        video_->setCropOverscan(on);
    });
    viewMenu_->addSeparator();
    fullScreenAct_ = viewMenu_->addAction(QString(), this, &MainWindow::toggleFullScreen);
    fullScreenAct_->setShortcutVisibleInContextMenu(true);

    optMenu_ = menuBar()->addMenu(QString());
    autoConnectAct_ = optMenu_->addAction(QString());
    autoConnectAct_->setCheckable(true);
    autoConnectAct_->setChecked(settings().value("autoConnect", true).toBool());
    connect(autoConnectAct_, &QAction::toggled, this, [](bool on) { settings().setValue("autoConnect", on); });
    onConnectSection_ = optMenu_->addSection(QString());
    auto *grp = new QActionGroup(this);
    resumeLastAct_ = optMenu_->addAction(QString());
    scanAct_ = optMenu_->addAction(QString());
    for (QAction *a : {resumeLastAct_, scanAct_}) {
        a->setCheckable(true);
        grp->addAction(a);
    }
    (settings().value("resumeLast", true).toBool() ? resumeLastAct_ : scanAct_)->setChecked(true);
    connect(resumeLastAct_, &QAction::toggled, this, [](bool on) { settings().setValue("resumeLast", on); });
    optMenu_->addSeparator();
    devModeAct_ = optMenu_->addAction(QString());
    devModeAct_->setCheckable(true);
    devModeAct_->setChecked(settings().value("developerMode", false).toBool());
    connect(devModeAct_, &QAction::toggled, this, [this](bool on) {
        settings().setValue("developerMode", on);
        applyDeveloperMode(on);
    });

    // Idioma: los nombres de cada idioma van siempre en ese idioma
    langMenu_ = menuBar()->addMenu(QString());
    langGroup_ = new QActionGroup(this);
    langSystemAct_ = langMenu_->addAction(QString());
    langSystemAct_->setData(QString());
    langMenu_->addSeparator();
    QAction *en = langMenu_->addAction(QStringLiteral("English"));
    en->setData(QStringLiteral("en"));
    QAction *es = langMenu_->addAction(QStringLiteral("Español"));
    es->setData(QStringLiteral("es"));
    const QString cur = i18n::setting();
    for (QAction *a : {langSystemAct_, en, es}) {
        a->setCheckable(true);
        langGroup_->addAction(a);
        if (a->data().toString() == cur) a->setChecked(true);
    }
    connect(langGroup_, &QActionGroup::triggered, this, [](QAction *a) { i18n::apply(a->data().toString()); });

    helpMenu_ = menuBar()->addMenu(QString());
    manualAct_ = helpMenu_->addAction(QString(), this, [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl))); });
    aboutAct_ = helpMenu_->addAction(QString(), this, &MainWindow::showAbout);
}

void MainWindow::chooseScanChannels() {
    ScanChannelsDialog dlg(scanChannels_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    scanChannels_ = dlg.selected();
    if (scanChannels_.isEmpty()) settings().remove("scanChannels");
    else settings().setValue("scanChannels", scanChannels_);
    emit requestScanChannels(ScanChannelsDialog::frequencies(scanChannels_));
    updateScanButton();
}

void MainWindow::updateScanButton() {
    const int n = int(ScanChannelsDialog::frequencies(scanChannels_).size());
    scanBtn_->setText(n == 0 ? tr("Scan 5.8 GHz") : n == 1 ? tr("Scan 1 channel") : tr("Scan %1 channels").arg(n));
    scanBtn_->setToolTip(n == 0 ? tr("Sweep the whole 5.3-5.9 GHz range") : scanChannels_.join(QStringLiteral(", ")));
    scanCfgBtn_->setToolTip(tr("Choose which channels to scan"));
}

void MainWindow::retranslate() {
    updateScanButton();
    stopBtn_->setText(tr("Stop"));
    updateRecordButton();
    updateMediaButtons();
    updateConnectButton();
    pilotsLabel_->setText(tr("Pilots found (click to watch):"));
    channels_->setItemText(0, tr("Manual channel..."));
    rate_->setItemText(0, tr("40 MS/s (USB 3)"));
    rate_->setToolTip(tr("Sample rate while watching a channel. 20 MS/s gives the same picture; "
                         "40 MS/s needs USB 3 and a fast host."));
    gain_->setItemText(0, tr("AGC"));
    gain_->setToolTip(tr("Receiver gain: automatic (AGC) or fixed."));
    rateLabel_->setText(tr("Sample rate"));
    gainLabel_->setText(tr("Gain"));

    fileMenu_->setTitle(tr("&File"));
    snapAct_->setText(tr("Take a &snapshot") + QStringLiteral("\tS"));
    snapDirMenu_->setTitle(tr("Snapshots folder"));
    videoDirMenu_->setTitle(tr("Videos folder"));
    for (QAction *a : {snapDirOpenAct_, videoDirOpenAct_}) a->setText(tr("Open"));
    for (QAction *a : {snapDirChangeAct_, videoDirChangeAct_}) a->setText(tr("Change..."));
    recDirAct_->setText(tr("Open &recordings folder"));
    logDirAct_->setText(tr("Open &log folder"));
    if (shortcutAct_) shortcutAct_->setText(tr("Create &desktop shortcut"));
    if (driverAct_) driverAct_->setText(tr("Install adapter &driver (WinUSB)..."));
    quitAct_->setText(tr("&Quit"));
    viewMenu_->setTitle(tr("&View"));
    aspectSection_->setText(tr("Picture shape"));
    aspectActs_[0]->setText(tr("4:3 (standard analog)"));
    aspectActs_[1]->setText(tr("16:9 (camera in widescreen mode)"));
    aspectActs_[2]->setText(tr("Stretch to the window"));
    cropAct_->setText(tr("Crop the borders (overscan)"));
    cropAct_->setToolTip(tr("Hide the dirty edges of the analog picture, as a TV does (3 % per side)"));
    fullScreenAct_->setText(tr("Full screen	F"));
    optMenu_->setTitle(tr("&Options"));
    autoConnectAct_->setText(tr("&Connect the adapter on startup"));
    devModeAct_->setText(tr("&Developer mode"));
    devModeAct_->setToolTip(tr("Shows sample rate, gain, IQ recording and the receiver's technical data"));
    onConnectSection_->setText(tr("On connect"));
    resumeLastAct_->setText(tr("Go back to the &last channel"));
    scanAct_->setText(tr("&Scan the bands"));
    langMenu_->setTitle(tr("&Language"));
    langSystemAct_->setText(tr("&System default"));
    helpMenu_->setTitle(tr("&Help"));
    manualAct_->setText(tr("&Manual and source code (GitHub)"));
    aboutAct_->setText(tr("&About %1").arg(kAppName));

    updatePlaceholder();
    onStatus(lastStatus_);
}

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) retranslate();
    QMainWindow::changeEvent(e);
}

void MainWindow::updateConnectButton() {
    connectBtn_->setText(opening_ ? tr("Connecting...") : open_ ? tr("Disconnect") : tr("Connect adapter"));
}

void MainWindow::updatePlaceholder() {
    switch (screen_) {
    case Screen::Idle:
        video_->setPlaceholder(tr("Press \"Connect adapter\" and then \"Scan\".\n\n"
                                  "F / double-click: full screen   O: OSD   Esc: leave full screen\n"
                                  "S: snapshot   R: record video"));
        break;
    case Screen::Connecting:
        video_->setPlaceholder(tr("Connecting to the adapter..."));
        break;
    case Screen::Ready:
        video_->setPlaceholder(tr("Adapter ready. Scan or pick a channel."));
        break;
    case Screen::Error:
        video_->setPlaceholder(tr("Could not open the adapter:\n%1\n\nPlug it in and press \"Connect adapter\".").arg(openError_));
        break;
    case Screen::Lost:
        video_->setPlaceholder(openError_);
        break;
    }
}

#ifdef _WIN32
void MainWindow::installDriver() {
    // scripts/windows-driver.ps1: en la version portable va en driver\, en una compilacion en el repositorio
    const QString dir = QApplication::applicationDirPath();
    QString script;
    for (const QString &c : {dir + QStringLiteral("/driver/windows-driver.ps1"),
                             dir + QStringLiteral("/../../scripts/windows-driver.ps1")})
        if (QFileInfo::exists(c)) {
            script = QFileInfo(c).absoluteFilePath();
            break;
        }
    if (script.isEmpty()) {
        QMessageBox::warning(this, kAppName, tr("The driver installer (driver\\windows-driver.ps1) was not found."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, kAppName,
        tr("This installs the WinUSB driver for the adapter, once. Windows will ask for administrator permission.\n\n"
           "It creates a local certificate to sign the driver (its private key is deleted right after signing) "
           "and adds it to the trusted stores of this PC; it replaces Zadig and survives unplugging the adapter. "
           "It can be removed with the same script and -Uninstall.\n\n"
           "Close this program after installing and unplug and replug the adapter. Continue?"));
    if (answer != QMessageBox::Yes) return;
    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -NoExit -File \"%1\" -RemoveZadig")
                               .arg(QDir::toNativeSeparators(script));
    const auto r = reinterpret_cast<intptr_t>(ShellExecuteW(
        reinterpret_cast<HWND>(winId()), L"runas", L"powershell.exe", reinterpret_cast<const wchar_t *>(params.utf16()),
        nullptr, SW_SHOWNORMAL));
    if (r <= 32) statusBar()->showMessage(tr("Driver installation cancelled"), 6000);
}
#endif

void MainWindow::showAbout() {
    QMessageBox box(this);
    box.setWindowTitle(tr("About %1").arg(kAppName));
    box.setIconPixmap(QPixmap(QStringLiteral(":/icons/app-128.png")));
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<h3>%1 %2</h3>"
                   "<p>Analog 5.8 GHz FPV video receiver with an RTL8812BU USB WiFi adapter, "
                   "for Windows and Linux.</p>"
                   "<p>It uses the same core as the Android app "
                   "<a href='https://github.com/gilankpam/kepyur'>kepyur</a> (gilankpam), with "
                   "<a href='https://github.com/gilankpam/wifi-sdr'>wifi-sdr</a>, "
                   "<a href='https://github.com/snokvist/devourer'>devourer</a> and libusb.</p>"
                   "<p>GPL-2.0 license &middot; <a href='%3'>%3</a></p>")
                    .arg(kAppName, KP_VERSION, QString::fromLatin1(kRepoUrl)));
    box.exec();
}

QString MainWindow::recordingsDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') + kAppName;
}

QString MainWindow::snapshotsDir() const {
    const QString def = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QLatin1Char('/') + kAppName;
    return settings().value("snapshotDir", def).toString();
}

QString MainWindow::videosDir() const {
    const QString def = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QLatin1Char('/') + kAppName;
    return settings().value("videoDir", def).toString();
}

void MainWindow::chooseMediaDir(const char *key, bool videos) {
    const QString cur = videos ? videosDir() : snapshotsDir();
    QDir().mkpath(cur);
    const QString d = QFileDialog::getExistingDirectory(
        this, videos ? tr("Folder for the videos") : tr("Folder for the snapshots"), cur);
    if (d.isEmpty()) return;
    settings().setValue(key, d);
    statusBar()->showMessage((videos ? tr("Videos will be saved in %1") : tr("Snapshots will be saved in %1"))
                                 .arg(QDir::toNativeSeparators(d)), 8000);
    updateMediaButtons();  // carpeta en los tooltips
}

QString MainWindow::mediaFileName(const QString &dir, const QString &ext) const {
    QString base = dir + QStringLiteral("/fpv-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (const auto *ch = watching_ > 0 ? kp::nearest_channel(watching_) : nullptr)
        base += QLatin1Char('-') + QString::fromLatin1(ch->name);
    QString f = base + ext;
    for (int i = 2; QFileInfo::exists(f); ++i) f = QStringLiteral("%1-%2%3").arg(base).arg(i).arg(ext);
    return f;
}

void MainWindow::takeSnapshot() {
    const QImage img = video_->hasLiveFrame() ? video_->snapshot() : QImage();
    if (img.isNull()) {
        statusBar()->showMessage(tr("No picture to capture"), 4000);
        return;
    }
    const QString dir = snapshotsDir();
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, kAppName, tr("Could not create the folder %1.").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    const QString path = mediaFileName(dir, QStringLiteral(".png"));
    if (QFile reserve(path); !reserve.open(QIODevice::WriteOnly)) {  // reservar el nombre (dos capturas en el mismo segundo)
        statusBar()->showMessage(tr("Could not save the snapshot to %1").arg(QDir::toNativeSeparators(path)), 10000);
        return;
    }
    // Comprimir el PNG lleva unas decenas de ms: fuera del hilo de la GUI para no saltarse fotogramas al pintar
    snapPool_.start([this, img, path] {
        const bool ok = img.save(path, "PNG");
        QMetaObject::invokeMethod(this, [this, ok, path] {
            if (ok) {
                statusBar()->showMessage(tr("Snapshot saved: %1").arg(QDir::toNativeSeparators(path)), 8000);
            } else {
                QFile::remove(path);
                statusBar()->showMessage(tr("Could not save the snapshot to %1").arg(QDir::toNativeSeparators(path)), 10000);
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::toggleVideoRecording() {
    if (dvr_->isRecording()) {
        backend_->setFrameTap({});
        dvr_->stop();  // termina el archivo en su hilo; luego onVideoRecorded()
        statusBar()->showMessage(tr("Finishing the video..."));
        return;
    }
    if (!dvrBtn_->isEnabled()) return;  // sin canal o sin codificador
    const QString dir = videosDir();
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, kAppName, tr("Could not create the folder %1.").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    const qint64 avail = QStorageInfo(dir).bytesAvailable();
    if (avail >= 0 && avail < 500LL * 1000 * 1000) {
        QMessageBox::warning(this, kAppName, tr("Only %1 MB free in %2: the video takes about 60 MB per minute.")
                                                 .arg(avail / 1000000).arg(QDir::toNativeSeparators(dir)));
        return;
    }
    const auto asp = video_->aspect();
    const double aspect = asp == VideoWidget::Aspect::R16x9 ? 16.0 / 9 : asp == VideoWidget::Aspect::R4x3 ? 4.0 / 3 : 0;
    const QString path = mediaFileName(dir, QStringLiteral(".mp4"));
    if (!dvr_->start(path, aspect)) return;
    VideoRecorder *rec = dvr_;
    backend_->setFrameTap([rec](const QImage &img, qint64 ns) { rec->push(img, ns); });
    statusBar()->showMessage(tr("Recording video to %1").arg(QDir::toNativeSeparators(path)), 8000);
    dvrTick_->start();
    dvrSpace_->start();
    updateMediaButtons();
}

void MainWindow::checkVideoSpace() {
    if (!dvr_->isRecording()) return;
    const qint64 avail = QStorageInfo(QFileInfo(dvr_->path()).absolutePath()).bytesAvailable();
    if (avail < 0 || avail >= 100LL * 1000 * 1000) return;
    toggleVideoRecording();  // parar: el MP4 queda bien cerrado
    QMessageBox::warning(this, kAppName, tr("The disk is almost full (%1 MB free): the video recording was stopped.\n\n%2")
                                             .arg(avail / 1000000).arg(QDir::toNativeSeparators(dvr_->path())));
}

void MainWindow::onVideoRecorded(const QString &path, bool ok, bool noPicture, const QString &error, int frames,
                                 int dropped, double seconds) {
    // Puede terminar por si solo (error al codificar o escribir): dejar de enviarle fotogramas
    backend_->setFrameTap({});
    dvrTick_->stop();
    dvrSpace_->stop();
    updateMediaButtons();
    const QString file = QDir::toNativeSeparators(path);
    if (ok) {
        QString m = tr("Video saved: %1 (%2 s, %3 MB)")
                        .arg(file)
                        .arg(seconds, 0, 'f', 1)
                        .arg(QFileInfo(path).size() / 1e6, 0, 'f', 1);
        if (dropped > 0) m += QStringLiteral("  ") + tr("%n frame(s) lost: the encoder could not keep up", nullptr, dropped);
        statusBar()->showMessage(m, 15000);
    } else if (noPicture) {
        statusBar()->showMessage(tr("No video recorded: no picture was received"), 10000);
    } else if (frames > 0) {
        QMessageBox::warning(this, kAppName, tr("The video recording stopped because of an error:\n%1\n\n"
                                                "What was recorded until then was saved in %2")
                                                 .arg(error, file));
    } else {
        QMessageBox::warning(this, kAppName, tr("The video recording failed:\n%1\n\n%2").arg(error, file));
    }
}

void MainWindow::updateMediaButtons() {
    const bool watchingNow = open_ && lastStatus_.open && lastStatus_.mode == QLatin1String("watch");
    snapBtn_->setText(tr("Snapshot"));
    snapBtn_->setEnabled(watchingNow);
    snapBtn_->setToolTip(tr("Save the current picture as PNG (S). Folder: %1").arg(QDir::toNativeSeparators(snapshotsDir())));
    const bool rec = dvr_->isRecording();
    if (rec) {
        const qint64 s = dvr_->elapsedMs() / 1000;
        const QString t = QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
        dvrBtn_->setText(QString(QChar(0x25A0)) + QLatin1Char(' ') + t);  // cuadrado de parar
        static const QString red = QStringLiteral("QPushButton{color:white;background:#c62828;font-weight:bold;}");
        if (dvrBtn_->styleSheet() != red) dvrBtn_->setStyleSheet(red);  // cada 250 ms: no repolir si no cambia
        dvrBtn_->setToolTip(tr("Recording to %1. Press again (R) to stop.").arg(QDir::toNativeSeparators(dvr_->path())));
        video_->setRecIndicator(QStringLiteral("REC ") + t);
        dvrAct_->setText(tr("Stop recording the &video") + QStringLiteral("\tR"));
    } else {
        dvrBtn_->setText(QString(QChar(0x25CF)) + QLatin1Char(' ') + tr("Record video"));  // punto de grabar
        if (!dvrBtn_->styleSheet().isEmpty()) dvrBtn_->setStyleSheet(QString());
        dvrBtn_->setToolTip(VideoRecorder::available()
                                ? tr("Record the picture to an MP4 file (R); press again to stop. Folder: %1")
                                      .arg(QDir::toNativeSeparators(videosDir()))
                                : tr("This build cannot record video: it was compiled without FFmpeg (libavcodec)."));
        video_->setRecIndicator(QString());
        dvrAct_->setText(tr("Start recording the &video") + QStringLiteral("\tR"));
    }
    // Para empezar hace falta estar viendo un canal; mientras graba siempre se puede parar
    dvrBtn_->setEnabled(rec || (watchingNow && VideoRecorder::available()));
    snapAct_->setEnabled(snapBtn_->isEnabled());
    dvrAct_->setEnabled(dvrBtn_->isEnabled());
}

void MainWindow::applyDeveloperMode(bool on) {
    devMode_ = on;
    devBox_->setVisible(on);
    recBtn_->setVisible(on);
    recDirAct_->setVisible(on);  // carpeta de las grabaciones IQ
    if (!on) {  // valores por defecto: lo que no se ve no puede quedar cambiado
        const bool changed = rate_->currentIndex() != kDefaultRateIndex || gain_->currentIndex() != 0;
        rate_->setCurrentIndex(kDefaultRateIndex);
        gain_->setCurrentIndex(0);  // AGC
        settings().setValue("rateIndex", kDefaultRateIndex);
        settings().setValue("gainIndex", 0);
        if (changed && open_) {
            emit requestWatchRate(rate_->currentData().toInt());
            emit requestGain(gain_->currentData().toDouble());
            if (watching_ > 0) emit requestWatch(watching_);
        }
    }
    onStatus(lastStatus_);  // datos tecnicos
}

void MainWindow::startup(bool autoConnect, double watchMHz, bool forceScan) {
    if (!autoConnect && watchMHz <= 0 && !forceScan && !autoConnectAct_->isChecked()) return;
    pendingWatch_ = watchMHz;
    pendingScan_ = forceScan;
    if (pendingWatch_ <= 0 && !pendingScan_) {
        const double last = settings().value("lastMHz", 0.0).toDouble();
        if (resumeLastAct_->isChecked() && last > 0) pendingWatch_ = last;
        else pendingScan_ = true;
    }
    connectAdapter(false);
}

void MainWindow::connectAdapter(bool userInitiated) {
    userConnect_ = userInitiated;
    opening_ = true;
    connectBtn_->setEnabled(false);
    updateConnectButton();
    screen_ = Screen::Connecting;
    updatePlaceholder();
    emit requestOpen();
}

void MainWindow::setRate(int rateSel) { rate_->setCurrentIndex(rateSel == 1 ? 0 : 1); }

void MainWindow::watchChannelFromCombo() {
    const double mhz = channels_->currentData().toDouble();
    if (mhz > 0) emit requestWatch(mhz);
}

void MainWindow::onOpened(bool ok, const QString &err) {
    opening_ = false;
    open_ = ok;
    connectBtn_->setEnabled(true);
    updateConnectButton();
    for (QWidget *w : {static_cast<QWidget *>(scanBtn_), static_cast<QWidget *>(stopBtn_),
                       static_cast<QWidget *>(channels_), static_cast<QWidget *>(list_)})
        w->setEnabled(ok);
    updateRecordButton();
    updateMediaButtons();
    if (ok) {
        statusBar()->showMessage(tr("Adapter ready"), 5000);
        screen_ = Screen::Ready;
        updatePlaceholder();
        emit requestWatchRate(rate_->currentData().toInt());
        emit requestGain(gain_->currentData().toDouble());
        if (pendingWatch_ > 0) emit requestWatch(pendingWatch_);
        else if (pendingScan_) emit requestScan();
        pendingWatch_ = 0;
        pendingScan_ = false;
        return;
    }
    pendingWatch_ = 0;
    pendingScan_ = false;
    if (err.isEmpty()) {
        screen_ = Screen::Idle;
        updatePlaceholder();
        return;
    }
    statusBar()->showMessage(tr("Could not open the adapter"));
    // Al abrir el programa (conexion automatica) no se interrumpe con un dialogo: el aviso queda en el video.
    openError_ = err;
    screen_ = Screen::Error;
    updatePlaceholder();
    if (userConnect_) QMessageBox::warning(this, kAppName, err);
}

void MainWindow::updateRecordButton() {
    // kepyur solo graba la senal que se esta viendo: sin canal sintonizado no haria nada
    const bool watchingNow = open_ && lastStatus_.open && lastStatus_.mode == QLatin1String("watch") && lastStatus_.msps > 0;
    recBtn_->setEnabled(watchingNow && recPrefix_.isEmpty());
    if (!recPrefix_.isEmpty()) {
        recBtn_->setText(tr("Recording..."));
    } else {
        recBtn_->setText(tr("Record IQ 5 s"));
        const double msps = lastStatus_.msps > 0 ? lastStatus_.msps : 20;
        recBtn_->setToolTip(tr("Saves 5 s of the raw signal of the channel being watched (about %1 MB at %2 MS/s), "
                               "for analysis or replay with kepyur's kp_replay.")
                                .arg(qRound(msps * 1e6 * 4 * kRecSeconds / 1e6))
                                .arg(msps, 0, 'f', 0));
    }
}

void MainWindow::startRecording() {
    if (lastStatus_.msps <= 0 || !recPrefix_.isEmpty()) return;
    const QString dir = recordingsDir();
    QDir().mkpath(dir);
    // complejos int16: 4 bytes por muestra (kepyur Radio::record_bank)
    const qint64 bytes = qint64(lastStatus_.msps * 1e6) * 4 * kRecSeconds;
    const qint64 avail = QStorageInfo(dir).bytesAvailable();
    if (avail >= 0 && avail < bytes + bytes / 5) {
        QMessageBox::warning(this, kAppName,
                             tr("Not enough free space in %1: the recording needs about %2 MB and there are %3 MB free.")
                                 .arg(QDir::toNativeSeparators(dir)).arg(bytes / 1000000).arg(avail / 1000000));
        return;
    }
    recPrefix_ = dir + "/rec-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    recStarted_.start();
    emit requestRecord(kRecSeconds, recPrefix_);
    statusBar()->showMessage(tr("Recording %1 s (about %2 MB) to %3.cs16...")
                                 .arg(kRecSeconds).arg(bytes / 1000000).arg(QDir::toNativeSeparators(recPrefix_)));
    recCheck_->start();
    updateRecordButton();
}

void MainWindow::checkRecording() {
    // kepyur escribe el .json al cerrar la grabacion: es la senal de que ha terminado bien
    const QFileInfo cs(recPrefix_ + ".cs16"), js(recPrefix_ + ".json");
    const bool done = js.exists();
    const bool timedOut = recStarted_.elapsed() > (kRecSeconds + 20) * 1000;
    if (!done && !timedOut) return;
    recCheck_->stop();
    if (done) {
        statusBar()->showMessage(tr("Recorded: %1 (%2 MB)").arg(QDir::toNativeSeparators(cs.absoluteFilePath()))
                                     .arg(cs.size() / 1000000), 15000);
    } else {
        const QString why = cs.exists() ? tr("incomplete, the channel was left while recording")
                                        : tr("the file could not be created, see the log (File > Open log folder)");
        statusBar()->showMessage(tr("Recording failed: %1").arg(why), 15000);
    }
    recPrefix_.clear();
    updateRecordButton();
}

void MainWindow::onLost(const QString &msg) {
    // El backend sigue intentando reconectar solo; "Conectar adaptador" queda disponible para hacerlo a mano.
    opening_ = false;
    open_ = false;
    connectBtn_->setEnabled(true);
    updateConnectButton();
    for (QWidget *w : {static_cast<QWidget *>(scanBtn_), static_cast<QWidget *>(stopBtn_),
                       static_cast<QWidget *>(channels_), static_cast<QWidget *>(list_)})
        w->setEnabled(false);
    updateRecordButton();
    updateMediaButtons();
    video_->clear();
    list_->clear();
    openError_ = msg;
    screen_ = Screen::Lost;
    updatePlaceholder();
    statusBar()->showMessage(tr("Adapter disconnected"));
}

void MainWindow::onPilots(const QVector<Pilot> &pilots) {
    const double sel = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toDouble() : 0;
    list_->clear();
    for (const Pilot &p : pilots) {
        auto *it = new QListWidgetItem(QStringLiteral("%1   %2 dB").arg(p.label()).arg(p.snrDb, 0, 'f', 1));
        it->setData(Qt::UserRole, p.freqMHz);
        list_->addItem(it);
        if (qAbs(p.freqMHz - sel) < 1) list_->setCurrentItem(it);
    }
}

void MainWindow::onStatus(const RadioStatus &s) {
    lastStatus_ = s;
    updateRecordButton();
    updateMediaButtons();
    watching_ = s.mode == QLatin1String("watch") ? s.watchMHz : 0;
    if (watching_ > 0 && qAbs(watching_ - lastSaved_) > 0.5 && s.chip != QLatin1String("demo")) {
        lastSaved_ = watching_;
        settings().setValue("lastMHz", watching_);
    }
    QString osd;
    if (s.mode == QLatin1String("scan")) {
        osd = tr("Scanning %1/%2").arg(s.scanStep + 1).arg(qMax(1, s.scanSteps));
    } else if (s.mode == QLatin1String("watch")) {
        const auto *ch = kp::nearest_channel(s.watchMHz);
        osd = QStringLiteral("%1%2 MHz").arg(ch ? QString::fromLatin1(ch->name) + "  " : QString())
                  .arg(s.watchMHz, 0, 'f', 0);
        if (s.snrDb >= 0) osd += QStringLiteral("  SNR %1 dB").arg(s.snrDb, 0, 'f', 0);
        osd += QStringLiteral("  ") + (s.locked ? (s.colour ? s.videoStd : tr("%1 B/W").arg(s.videoStd)) : tr("no sync"));
    }
    video_->setOsd(osd);

    QString mode = s.mode;
    if (s.mode == QLatin1String("idle")) mode = tr("idle");
    else if (s.mode == QLatin1String("scan")) mode = tr("scan");
    else if (s.mode == QLatin1String("watch")) mode = tr("watch");
    else if (s.mode == QLatin1String("test")) mode = tr("test");

    QString t;
    if (s.open && devMode_) {
        t += QStringLiteral("chip: %1  USB %2\n").arg(s.chip, s.usb3 ? "3" : "2");
        t += tr("mode: %1  rx: %2").arg(mode, s.rxState) + QLatin1Char('\n');
        if (s.mode == QLatin1String("watch"))
            t += tr("%1 MS/s%2  lock %3%  %4 fps")
                     .arg(s.msps, 0, 'f', 0)
                     .arg(s.fallback ? tr(" (fallback)") : QString())
                     .arg(s.lock * 100, 0, 'f', 0)
                     .arg(s.fps, 0, 'f', 1) + QLatin1Char('\n');
        // Al escanear, cada cambio de frecuencia reinicia el stream: el contador solo dice algo al ver un canal.
        if (s.mode == QLatin1String("watch"))
            t += tr("dropped: %1  restarts: %2").arg(s.dropped).arg(s.restarts);
        else
            t += tr("dropped: %1").arg(s.dropped);
    }
    // Errores y avisos: tambien sin el modo desarrollador
    if (s.open && !s.rxError.isEmpty()) t += (t.isEmpty() ? QString() : QStringLiteral("\n")) + tr("error: %1").arg(s.rxError);
    if (s.open && !s.usb3 && s.chip != QLatin1String("demo"))
        t += (t.isEmpty() ? QString() : QStringLiteral("\n")) + tr("WARNING: USB 2, no 40 MS/s. Use a USB 3 port.");
    info_->setText(t);
}

void MainWindow::toggleFullScreen() {
    const bool fs = !isFullScreen();
    side_->setVisible(!fs);
    statusBar()->setVisible(!fs);
    menuBar()->setVisible(!fs);
    if (fs) showFullScreen();
    else showNormal();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (isFullScreen()) toggleFullScreen();
    if (dvr_->isRecording()) {  // terminar el MP4 (sin su indice no se podria abrir); el backend aun existe aqui
        backend_->setFrameTap({});
        dvr_->waitFinished();
    }
    snapPool_.waitForDone();  // capturas PNG a medio guardar: terminarlas antes de destruir la ventana
    settings().setValue("geometry", saveGeometry());
    settings().sync();
    e->accept();
}

void MainWindow::setupShortcuts() {
    // Acciones de la ventana (no keyPressEvent): Qt resuelve los atajos antes de entregar la tecla al widget con
    // foco, asi funcionan aunque el foco este en la lista de pilotos o en un desplegable (que buscan por letra).
    // Anadidas a la propia ventana y no a un menu: la barra de menus se oculta en pantalla completa.
    auto add = [this](std::initializer_list<QKeySequence> keys, auto fn) {
        auto *a = new QAction(this);
        a->setShortcuts(QList<QKeySequence>(keys));
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
    };
    add({QKeySequence(Qt::Key_F), QKeySequence(Qt::Key_F11)}, [this] { toggleFullScreen(); });
    add({QKeySequence(Qt::Key_Escape)}, [this] { if (isFullScreen()) toggleFullScreen(); });
    add({QKeySequence(Qt::Key_O)}, [this] { video_->setOsdVisible(!video_->osdVisible()); });
    add({QKeySequence(Qt::Key_S)}, [this] { takeSnapshot(); });
    add({QKeySequence(Qt::Key_R)}, [this] { toggleVideoRecording(); });
}
