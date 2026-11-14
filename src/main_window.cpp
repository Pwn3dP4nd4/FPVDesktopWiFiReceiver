#include "main_window.h"

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
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

#include "i18n.h"
#include "spectrum.h"  // kp::fpv_channels(): la tabla de canales de kepyur
#include "video_widget.h"

namespace {
const char *kAppName = "FPVDesktopWiFiReceiver";
const char *kRepoUrl = "https://github.com/Pwn3dP4nd4/FPVDesktopWiFiReceiver";
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
    left->addWidget(connectBtn_);
    left->addWidget(scanBtn_);

    pilotsLabel_ = new QLabel;
    left->addWidget(pilotsLabel_);
    list_ = new QListWidget;
    left->addWidget(list_, 1);

    channels_ = new QComboBox;
    channels_->addItem(QString(), 0.0);  // "Canal manual..." (retranslate)
    for (const auto &c : kp::fpv_channels())
        channels_->addItem(QStringLiteral("%1  %2 MHz").arg(c.name).arg(c.mhz, 0, 'f', 0), c.mhz);
    left->addWidget(channels_);

    auto *form = new QFormLayout;
    rate_ = new QComboBox;
    rate_->addItem(QString(), 1);
    rate_->addItem(QStringLiteral("20 MS/s"), 2);
#ifdef _WIN32
    // WinUSB no sostiene de forma estable las ~24.400 transferencias/s de 40 MS/s (la version web de kepyur
    // tambien ve a 20). Misma imagen: el filtro RX deja +-10 MHz en ambos casos. El escaneo sigue a 40 MS/s.
    const int defaultRate = 1;
#else
    const int defaultRate = 0;
#endif
    rate_->setCurrentIndex(qBound(0, settings().value("rateIndex", defaultRate).toInt(), 1));
    gain_ = new QComboBox;
    gain_->addItem(QString(), -1.0);  // "AGC"
    for (int db : {10, 20, 30, 40, 50}) gain_->addItem(QStringLiteral("%1 dB").arg(db), double(db));
    gain_->setCurrentIndex(qBound(0, settings().value("gainIndex", 0).toInt(), gain_->count() - 1));
    rateLabel_ = new QLabel;
    gainLabel_ = new QLabel;
    form->addRow(rateLabel_, rate_);
    form->addRow(gainLabel_, gain_);
    left->addLayout(form);

    left->addWidget(stopBtn_);
    left->addWidget(recBtn_);

    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setStyleSheet(QStringLiteral("QLabel{font-family:monospace;font-size:11px;}"));
    left->addWidget(info_);

    video_ = new VideoWidget;
    video_->setLogo(QPixmap(QStringLiteral(":/icons/app-128.png")));
    h->addWidget(side_);
    h->addWidget(video_, 1);
    setCentralWidget(central);
    buildMenus();

    // GUI -> backend (otro hilo: conexiones en cola)
    connect(this, &MainWindow::requestOpen, backend, &Backend::openDevice);
    connect(this, &MainWindow::requestClose, backend, &Backend::closeDevice);
    connect(this, &MainWindow::requestScan, backend, &Backend::startScan);
    connect(this, &MainWindow::requestWatch, backend, &Backend::watch);
    connect(this, &MainWindow::requestStop, backend, &Backend::stop);
    connect(this, &MainWindow::requestWatchRate, backend, &Backend::setWatchRate);
    connect(this, &MainWindow::requestGain, backend, &Backend::setGain);
    connect(this, &MainWindow::requestRecord, backend, &Backend::record);
    // backend -> GUI
    connect(backend, &Backend::frameReady, this, [this](const QImage &img) {
        video_->setFrame(img);
        backend_->frameConsumed();
    });
    connect(backend, &Backend::opened, this, &MainWindow::onOpened);
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
    connect(recBtn_, &QPushButton::clicked, this, [this] {
        const QString dir = recordingsDir();
        QDir().mkpath(dir);
        const QString prefix = dir + "/rec-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        emit requestRecord(5.0, prefix);
        statusBar()->showMessage(tr("Recording 5 s to %1.cs16 (+ .json)").arg(QDir::toNativeSeparators(prefix)), 10000);
    });
    connect(video_, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullScreen);

    retranslate();
    onOpened(false, {});
    if (!restoreGeometry(settings().value("geometry").toByteArray())) resize(1100, 660);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenus() {
    fileMenu_ = menuBar()->addMenu(QString());
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
#endif
    fileMenu_->addSeparator();
    quitAct_ = fileMenu_->addAction(QString(), QKeySequence::Quit, this, &QWidget::close);

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

void MainWindow::retranslate() {
    scanBtn_->setText(tr("Scan 5.8 GHz"));
    stopBtn_->setText(tr("Stop"));
    recBtn_->setText(tr("Record IQ 5 s"));
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
    recDirAct_->setText(tr("Open &recordings folder"));
    logDirAct_->setText(tr("Open &log folder"));
    if (shortcutAct_) shortcutAct_->setText(tr("Create &desktop shortcut"));
    quitAct_->setText(tr("&Quit"));
    optMenu_->setTitle(tr("&Options"));
    autoConnectAct_->setText(tr("&Connect the adapter on startup"));
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
                                  "F / double-click: full screen   O: OSD   Esc: leave full screen"));
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
    }
}

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
                       static_cast<QWidget *>(recBtn_), static_cast<QWidget *>(channels_),
                       static_cast<QWidget *>(list_)})
        w->setEnabled(ok);
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
    if (s.open) {
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
        if (!s.rxError.isEmpty()) t += QLatin1Char('\n') + tr("error: %1").arg(s.rxError);
        if (!s.usb3 && s.chip != QLatin1String("demo"))
            t += QLatin1Char('\n') + tr("WARNING: USB 2, no 40 MS/s. Use a USB 3 port.");
    }
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
    settings().setValue("geometry", saveGeometry());
    settings().sync();
    e->accept();
}

void MainWindow::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_F: toggleFullScreen(); return;
    case Qt::Key_Escape: if (isFullScreen()) toggleFullScreen(); return;
    case Qt::Key_O: video_->setOsdVisible(!video_->osdVisible()); return;
    default: QMainWindow::keyPressEvent(e);
    }
}
