#include "main_window.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>

#include "spectrum.h"  // kp::fpv_channels(): la tabla de canales de kepyur
#include "video_widget.h"

MainWindow::MainWindow(Backend *backend, QWidget *parent) : QMainWindow(parent), backend_(backend) {
    setWindowTitle(QStringLiteral("kepyur desktop %1 [%2]").arg(KP_VERSION, backend->name()));

    auto *central = new QWidget;
    auto *h = new QHBoxLayout(central);
    h->setContentsMargins(6, 6, 6, 6);

    side_ = new QWidget;
    side_->setFixedWidth(250);
    auto *left = new QVBoxLayout(side_);
    left->setContentsMargins(0, 0, 0, 0);

    connectBtn_ = new QPushButton(tr("Conectar adaptador"));
    scanBtn_ = new QPushButton(tr("Escanear 5.8 GHz"));
    stopBtn_ = new QPushButton(tr("Parar"));
    left->addWidget(connectBtn_);
    left->addWidget(scanBtn_);

    left->addWidget(new QLabel(tr("Pilotos encontrados (clic = ver):")));
    list_ = new QListWidget;
    left->addWidget(list_, 1);

    channels_ = new QComboBox;
    channels_->addItem(tr("Canal manual..."), 0.0);
    for (const auto &c : kp::fpv_channels())
        channels_->addItem(QStringLiteral("%1  %2 MHz").arg(c.name).arg(c.mhz, 0, 'f', 0), c.mhz);
    left->addWidget(channels_);

    auto *form = new QFormLayout;
    rate_ = new QComboBox;
    rate_->addItem(tr("40 MS/s (USB 3)"), 1);
    rate_->addItem(tr("20 MS/s"), 2);
#ifdef _WIN32
    // WinUSB no sostiene de forma estable las ~24.400 transferencias/s de 40 MS/s (la version web de kepyur
    // tambien ve a 20). Misma imagen: el filtro RX deja +-10 MHz en ambos casos. El escaneo sigue a 40 MS/s.
    rate_->setCurrentIndex(1);
#endif
    rate_->setToolTip(tr("Muestreo al ver un canal. 20 MS/s da la misma imagen; 40 MS/s necesita USB 3 y un host rapido."));
    gain_ = new QComboBox;
    gain_->addItem(tr("AGC"), -1.0);
    for (int db : {10, 20, 30, 40, 50}) gain_->addItem(QStringLiteral("%1 dB").arg(db), double(db));
    form->addRow(tr("Muestreo"), rate_);
    form->addRow(tr("Ganancia"), gain_);
    left->addLayout(form);

    recBtn_ = new QPushButton(tr("Grabar IQ 5 s"));
    left->addWidget(stopBtn_);
    left->addWidget(recBtn_);

    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_->setStyleSheet(QStringLiteral("QLabel{font-family:monospace;font-size:11px;}"));
    left->addWidget(info_);

    video_ = new VideoWidget;
    video_->setPlaceholder(tr("Pulsa \"Conectar adaptador\" y luego \"Escanear\".\n\n"
                              "F / doble clic: pantalla completa   O: OSD   Esc: salir de pantalla completa"));
    h->addWidget(side_);
    h->addWidget(video_, 1);
    setCentralWidget(central);

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
        opening_ = true;
        connectBtn_->setEnabled(false);
        connectBtn_->setText(tr("Conectando..."));
        emit requestOpen();
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
        emit requestWatchRate(rate_->currentData().toInt());
        if (watching_ > 0) emit requestWatch(watching_);  // la tasa se aplica en el siguiente watch()
    });
    connect(gain_, &QComboBox::activated, this, [this] {
        emit requestGain(gain_->currentData().toDouble());
        if (watching_ > 0) emit requestWatch(watching_);
    });
    connect(recBtn_, &QPushButton::clicked, this, [this] {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/kepyur";
        QDir().mkpath(dir);
        const QString prefix = dir + "/rec-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
        emit requestRecord(5.0, prefix);
        statusBar()->showMessage(tr("Grabando 5 s en %1.cs16 (+ .json)").arg(QDir::toNativeSeparators(prefix)), 10000);
    });
    connect(video_, &VideoWidget::doubleClicked, this, &MainWindow::toggleFullScreen);

    onOpened(false, {});
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
    connectBtn_->setText(ok ? tr("Desconectar") : tr("Conectar adaptador"));
    for (QWidget *w : {static_cast<QWidget *>(scanBtn_), static_cast<QWidget *>(stopBtn_),
                       static_cast<QWidget *>(recBtn_), static_cast<QWidget *>(channels_),
                       static_cast<QWidget *>(list_)})
        w->setEnabled(ok);
    if (ok) {
        statusBar()->showMessage(tr("Adaptador listo"), 5000);
        emit requestWatchRate(rate_->currentData().toInt());
    }
    if (!ok && !err.isEmpty()) {
        statusBar()->showMessage(tr("No se pudo abrir el adaptador"));
        QMessageBox::warning(this, tr("kepyur"), err);
    }
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
    watching_ = s.mode == QLatin1String("watch") ? s.watchMHz : 0;
    QString osd;
    if (s.mode == QLatin1String("scan")) {
        osd = tr("Escaneando %1/%2").arg(s.scanStep + 1).arg(qMax(1, s.scanSteps));
    } else if (s.mode == QLatin1String("watch")) {
        const auto *ch = kp::nearest_channel(s.watchMHz);
        osd = QStringLiteral("%1%2 MHz").arg(ch ? QString::fromLatin1(ch->name) + "  " : QString())
                  .arg(s.watchMHz, 0, 'f', 0);
        if (s.snrDb >= 0) osd += QStringLiteral("  SNR %1 dB").arg(s.snrDb, 0, 'f', 0);
        osd += s.locked ? QStringLiteral("  %1%2").arg(s.videoStd, s.colour ? "" : " B/N") : tr("  sin sincronismo");
    }
    video_->setOsd(osd);

    QString t;
    if (s.open) {
        t += QStringLiteral("chip: %1  USB %2\n").arg(s.chip, s.usb3 ? "3" : "2");
        t += QStringLiteral("modo: %1  rx: %2\n").arg(s.mode, s.rxState);
        if (s.mode == QLatin1String("watch"))
            t += QStringLiteral("%1 MS/s%2  lock %3%  %4 fps\n")
                     .arg(s.msps, 0, 'f', 0)
                     .arg(s.fallback ? " (fallback)" : "")
                     .arg(s.lock * 100, 0, 'f', 0)
                     .arg(s.fps, 0, 'f', 1);
        t += QStringLiteral("perdidos: %1  reinicios: %2").arg(s.dropped).arg(s.restarts);
        if (!s.rxError.isEmpty()) t += QStringLiteral("\nerror: ") + s.rxError;
        if (!s.usb3 && s.chip != QLatin1String("demo"))
            t += tr("\nAVISO: USB 2, sin 40 MS/s. Usa un puerto USB 3.");
    }
    info_->setText(t);
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        showNormal();
        side_->show();
        statusBar()->show();
    } else {
        side_->hide();
        statusBar()->hide();
        showFullScreen();
    }
}

void MainWindow::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_F: toggleFullScreen(); return;
    case Qt::Key_Escape: if (isFullScreen()) toggleFullScreen(); return;
    case Qt::Key_O: video_->setOsdVisible(!video_->osdVisible()); return;
    default: QMainWindow::keyPressEvent(e);
    }
}
