#include "elrs_analyzer.h"
#include "app_settings.h"
#include "log.h"

#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QPixmap>
#include <QScrollBar>
#include <QTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {
// La banda que se barre: lo que el adaptador alcanza a 2,4 GHz (src/scan_plan.h, survey_plan_24)
constexpr double kSurveyLo = 2402, kSurveyHi = 2482;

QColor levelColour(elrs::Level l) {
    switch (l) {
    case elrs::Level::Clean: return QColor(0x3c, 0xb0, 0x4a);
    case elrs::Level::Busy: return QColor(0xe8, 0xb0, 0x20);
    case elrs::Level::Bad: return QColor(0xd8, 0x3c, 0x30);
    case elrs::Level::Unknown: break;
    }
    return QColor(0x90, 0x90, 0x90);
}

const QColor kWifiColour(0x40, 0x80, 0xe0);
// la barra de barrido, siempre azul (el color de resalte del sistema puede ser cualquiera: gris, verde...)
const QColor kSweepColour(0x1e, 0x78, 0xe6);

QString pct(double f) {
    const double v = f * 100;
    if (v > 0 && v < 0.01) return QStringLiteral("< 0.01 %");
    return QStringLiteral("%1 %").arg(QLocale().toString(v, 'f', v < 1 ? 2 : v < 10 ? 1 : 0));
}

// datos de la ventana -> del recomendador (solo la banda de 2,4 GHz)
std::vector<elrs::Bin> toBins(const QVector<SpectrumPoint> &in) {
    std::vector<elrs::Bin> out;
    out.reserve(size_t(in.size()));
    for (const auto &p : in)
        if (p.mhz > 2300 && p.mhz < 2600) out.push_back({p.mhz, p.meanDb, p.peakDb, p.duty});
    return out;
}
}  // namespace

// ---- grafica ------------------------------------------------------------------------------------------------

ElrsView::ElrsView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    clock_.start();
    anim_ = new QTimer(this);
    anim_->setInterval(60);
    connect(anim_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void ElrsView::setScanning(bool on) {
    scanning_ = on;
    if (on) {
        cellSeq_.clear();
        cellT_.clear();
        anim_->start();
    } else {
        anim_->stop();
    }
    update();
}

void ElrsView::setData(const QVector<SpectrumPoint> &bins, const elrs::Report &r) {
    // seq crece con cada paso medido: un MHz cuyo seq ha cambiado se acaba de volver a medir
    const qint64 t = clock_.elapsed();
    for (const auto &b : bins) {
        if (b.seq == 0) continue;
        const int c = int(std::floor(b.mhz));
        const auto it = cellSeq_.find(c);
        if (it != cellSeq_.end() && it.value() == b.seq) continue;
        cellSeq_[c] = b.seq;
        cellT_[c] = t;
    }
    bins_ = bins;
    report_ = r;
    update();
}

void ElrsView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());
    const QFontMetrics fm(font());
    Geometry g{fm};
    g.left = fm.horizontalAdvance(QStringLiteral("ELRS")) + 12;
    g.right = width() - 8;
    g.wifiRowH = fm.height() + 4;
    g.elrsRowH = fm.height() + 6;
    g.barTop = 5;
    g.barH = 6;
    g.top = g.barTop + g.barH + 6 + g.wifiRowH + 4;
    g.plotBottom = height() - g.elrsRowH - fm.height() - 12;
    drawGrid(p, g);
    drawSweepBar(p, g);
    drawUnreachable(p, g);
    drawWifi(p, g);
    drawSpectrum(p, g);
    drawAxis(p, g);
    drawChannels(p, g);
}

void ElrsView::drawGrid(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    QColor grid = pal.color(QPalette::Text);
    grid.setAlpha(70);
    for (int db = 10; db <= 40; db += 10) {
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(g.left, g.y(db)), QPointF(g.right, g.y(db)));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, g.y(db) - g.fm.height() / 2.0, g.left - 4, g.fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(db));
    }
}

// barra de barrido: cada MHz, brillante al volver a medirse y apagandose; donde va ahora, una franja tenue
void ElrsView::drawSweepBar(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    const qint64 t = clock_.elapsed();
    QColor off = pal.color(QPalette::Text);
    off.setAlpha(30);
    for (int c = int(kSurveyLo); c < int(kSurveyHi); c++) {
        const QRectF cell(QPointF(g.x(c), g.barTop), QPointF(g.x(c + 1), g.barTop + g.barH));
        const auto it = cellT_.find(c);
        const double a = it == cellT_.end() ? 0 : std::clamp(1.0 - (t - it.value()) / double(kFadeMs), 0.0, 1.0);
        if (it == cellT_.end()) {
            p.fillRect(cell, off);
            continue;
        }
        QColor on = kSweepColour;
        on.setAlpha(60 + int(195 * a));
        p.fillRect(cell, on);
        if (scanning_ && a > 0.75) {
            QColor head = kSweepColour;
            head.setAlpha(int(45 * (a - 0.75) / 0.25));
            p.fillRect(QRectF(QPointF(g.x(c), g.barTop + g.barH), QPointF(g.x(c + 1), height() - g.elrsRowH - 4)), head);
        }
    }
}

// lo que el adaptador no alcanza: sombreado
void ElrsView::drawUnreachable(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    QColor shade = pal.color(QPalette::Text);
    shade.setAlpha(25);
    p.fillRect(QRectF(QPointF(g.x(kLo), g.top), QPointF(g.x(kSurveyLo), g.plotBottom)), shade);
    p.fillRect(QRectF(QPointF(g.x(kSurveyHi), g.top), QPointF(g.x(kHi), g.plotBottom)), shade);
}

// canales WiFi detectados: su ancho, tenue, y su numero arriba
void ElrsView::drawWifi(QPainter &p, const Geometry &g) const {
    for (const auto &w : report_.wifi) {
        QColor c = kWifiColour;
        c.setAlpha(w.traffic ? 55 : 30);
        const QRectF band(QPointF(g.x(w.mhz - 8.3), g.top), QPointF(g.x(w.mhz + 8.3), g.plotBottom));
        p.fillRect(band, c);
        c.setAlpha(220);
        p.setPen(QPen(c, 1.5));
        p.drawLine(QPointF(band.left(), g.top - 2), QPointF(band.right(), g.top - 2));
        p.drawText(QRectF(band.left(), g.barTop + g.barH + 4, band.width(), g.wifiRowH), Qt::AlignCenter,
                   QStringLiteral("WiFi %1").arg(w.ch));
    }
}

// espectro: rafagas (fondo naranja), picos (linea) y media (area)
void ElrsView::drawSpectrum(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    for (const auto &b : bins_) {
        if (b.mhz < kLo || b.mhz > kHi || b.duty < 0.02f) continue;
        p.fillRect(QRectF(QPointF(g.x(b.mhz - 0.08), g.top), QPointF(g.x(b.mhz + 0.08), g.plotBottom)),
                   QColor(255, 140, 0, 20 + int(std::min(1.f, b.duty * 2) * 60)));
    }
    QPainterPath peak, mean;
    bool first = true;
    double lastMhz = 0;
    for (const auto &b : bins_) {
        if (b.mhz < kLo || b.mhz > kHi) continue;
        const double x = g.x(b.mhz);
        if (first || b.mhz - lastMhz > 0.5) {  // hueco (sin datos): otro tramo
            if (!first) mean.lineTo(g.x(lastMhz), g.plotBottom);
            peak.moveTo(x, g.y(b.peakDb));
            mean.moveTo(x, g.plotBottom);
        } else {
            peak.lineTo(x, g.y(b.peakDb));
        }
        mean.lineTo(x, g.y(b.meanDb));
        first = false;
        lastMhz = b.mhz;
    }
    if (!first) mean.lineTo(g.x(lastMhz), g.plotBottom);
    p.setPen(QPen(QColor(0xe0, 0x70, 0x30), 1));
    p.drawPath(peak);
    QColor fill = pal.color(QPalette::Highlight);
    p.setPen(QPen(fill, 1.5));
    fill.setAlpha(90);
    p.setBrush(fill);
    p.drawPath(mean);
    p.setBrush(Qt::NoBrush);
}

// eje de frecuencias
void ElrsView::drawAxis(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    p.setPen(pal.color(QPalette::Text));
    p.drawLine(QPointF(g.left, g.plotBottom), QPointF(g.right, g.plotBottom));
    for (int f = 2400; f <= kHi; f += 10) {
        p.drawLine(QPointF(g.x(f), g.plotBottom), QPointF(g.x(f), g.plotBottom + 3));
        p.drawText(QRectF(g.x(f) - 30, g.plotBottom + 3, 60, g.fm.height()), Qt::AlignCenter, QString::number(f));
    }
}

// los 80 canales de ELRS
void ElrsView::drawChannels(QPainter &p, const Geometry &g) const {
    const double rowTop = g.plotBottom + g.fm.height() + 8;
    p.drawText(QRectF(0, rowTop, g.left - 4, g.elrsRowH), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("ELRS"));
    for (const auto &c : report_.channels) {
        const QRectF cell(QPointF(g.x(c.mhz - 0.45), rowTop), QPointF(g.x(c.mhz + 0.45), rowTop + g.elrsRowH));
        p.fillRect(cell, levelColour(c.measured ? c.level : elrs::Level::Unknown));
    }
    if (report_.channels.empty())
        for (int i = 0; i < elrs::kChannels; i++) {
            const double m = elrs::channel_mhz(i);
            p.fillRect(QRectF(QPointF(g.x(m - 0.45), rowTop), QPointF(g.x(m + 0.45), rowTop + g.elrsRowH)),
                       levelColour(elrs::Level::Unknown));
        }
}

// ---- ventana --------------------------------------------------------------------------------------------------

ElrsAnalyzer::ElrsAnalyzer(Backend *backend, QWidget *parent) : QDialog(parent) {
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    auto *v = new QVBoxLayout(this);

    auto *row = new QHBoxLayout;
    useLbl_ = new QLabel;
    use_ = new QComboBox;
    for (int i = 0; i < 3; i++) use_->addItem(QString(), i);
    use_->setCurrentIndex(qBound(0, appSettings().value("elrs/use", 1).toInt(), 2));
    regionLbl_ = new QLabel;
    region_ = new QComboBox;
    for (int i = 0; i < 2; i++) region_->addItem(QString(), i);
    region_->setCurrentIndex(qBound(0, appSettings().value("elrs/region", 0).toInt(), 1));
    pilotsLbl_ = new QLabel;
    pilots_ = new QSpinBox;
    pilots_->setObjectName(QStringLiteral("pilots"));
    pilots_->setRange(0, 30);
    pilots_->setValue(appSettings().value("elrs/pilots", 0).toInt());
    memoryLbl_ = new QLabel;
    memory_ = new QComboBox;
    for (int n : {5, 10, 20, 40}) memory_->addItem(QString(), n);
    memory_->setCurrentIndex(qBound(0, memory_->findData(appSettings().value("elrs/memory", 20).toInt()), 3));
    startBtn_ = new QPushButton;
    startBtn_->setObjectName(QStringLiteral("start"));
    startBtn_->setEnabled(false);
    autoStop_ = new QCheckBox;
    autoStop_->setObjectName(QStringLiteral("autoStop"));
    autoStop_->setChecked(appSettings().value("elrs/autoStop", true).toBool());
    row->addWidget(useLbl_);
    row->addWidget(use_);
    row->addSpacing(12);
    row->addWidget(regionLbl_);
    row->addWidget(region_);
    row->addSpacing(12);
    row->addWidget(pilotsLbl_);
    row->addWidget(pilots_);
    row->addSpacing(12);
    row->addWidget(memoryLbl_);
    row->addWidget(memory_);
    row->addStretch(1);
    row->addWidget(autoStop_);
    row->addWidget(startBtn_);
    v->addLayout(row);

    state_ = new QLabel;
    state_->setWordWrap(true);
    v->addWidget(state_);

    auto *split = new QSplitter(Qt::Vertical);
    auto *top = new QWidget;
    auto *tv = new QVBoxLayout(top);
    tv->setContentsMargins(0, 0, 0, 0);
    view_ = new ElrsView;
    tv->addWidget(view_, 1);
    buildLegend();
    auto *legend = new QGridLayout;
    legend->setContentsMargins(4, 2, 4, 0);
    legend->setHorizontalSpacing(6);
    legend->setVerticalSpacing(2);
    {
        // fila 0: la grafica; fila 1: los canales de ELRS. Cada muestra, su texto al lado (legendText_, en orden)
        const auto pix = [](int kind, const QPalette &pal) {
            QPixmap px(22, 12);
            px.fill(Qt::transparent);
            QPainter q(&px);
            const QRect r(0, 0, 22, 12);
            QColor hl = pal.color(QPalette::Highlight);
            switch (kind) {
            case 0: hl.setAlpha(90); q.fillRect(r.adjusted(0, 4, 0, 0), hl); q.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
                    q.drawLine(0, 4, 22, 4); break;                                                    // media
            case 1: q.setPen(QPen(QColor(0xe0, 0x70, 0x30), 2)); q.drawLine(0, 8, 7, 3); q.drawLine(7, 3, 14, 9);
                    q.drawLine(14, 9, 22, 2); break;                                                   // picos
            case 2: q.fillRect(r, QColor(255, 140, 0, 70)); break;                                     // rafagas
            case 3: { QColor c = kWifiColour; c.setAlpha(60); q.fillRect(r, c); c.setAlpha(220); q.setPen(QPen(c, 2));
                    q.drawLine(0, 1, 22, 1); break; }                                                 // WiFi
            case 4: q.fillRect(r.adjusted(0, 3, 0, -3), kSweepColour); break;                                    // barrido
            default: q.fillRect(r.adjusted(5, 0, -5, 0), levelColour(static_cast<elrs::Level>(kind - 5)));  // niveles
            }
            return px;
        };
        legendGraph_ = new QLabel;
        legendElrs_ = new QLabel;
        legend->addWidget(legendGraph_, 0, 0);
        legend->addWidget(legendElrs_, 1, 0);
        // 0 media, 1 picos, 2 rafagas, 3 WiFi, 4 barrido | 5+: niveles (Unknown, Clean, Busy, Bad) en el orden limpio..sin medir
        const int graph[] = {0, 1, 2, 3, 4};
        const int levels[] = {5 + int(elrs::Level::Clean), 5 + int(elrs::Level::Busy), 5 + int(elrs::Level::Bad),
                              5 + int(elrs::Level::Unknown)};
        int col = 1;
        for (int k : graph) {
            auto *sw = new QLabel;
            sw->setPixmap(pix(k, palette()));
            auto *tx = new QLabel;
            legendText_ << tx;
            legend->addWidget(sw, 0, col);
            legend->addWidget(tx, 0, col + 1);
            col += 2;
        }
        col = 1;
        for (int k : levels) {
            auto *sw = new QLabel;
            sw->setPixmap(pix(k, palette()));
            auto *tx = new QLabel;
            legendText_ << tx;
            legend->addWidget(sw, 1, col);
            legend->addWidget(tx, 1, col + 1);
            col += 2;
        }
        legend->setColumnStretch(col + 8, 1);
    }
    tv->addLayout(legend);
    legendNote_ = new QLabel;
    legendNote_->setWordWrap(true);
    {
        QPalette np = legendNote_->palette();
        QColor c = np.color(QPalette::WindowText);
        c.setAlpha(150);
        np.setColor(QPalette::WindowText, c);
        legendNote_->setPalette(np);
        QFont f = legendNote_->font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        legendNote_->setFont(f);
    }
    tv->addWidget(legendNote_);
    summary_ = new QLabel;
    summary_->setObjectName(QStringLiteral("summary"));
    summary_->setWordWrap(true);
    QFont bold = summary_->font();
    bold.setBold(true);
    summary_->setFont(bold);
    tv->addWidget(summary_);
    split->addWidget(top);

    auto *bottom = new QWidget;
    auto *bh = new QHBoxLayout(bottom);
    bh->setContentsMargins(0, 0, 0, 0);
    auto *dl = new QVBoxLayout;
    detectedTitle_ = new QLabel;
    detectedTitle_->setFont(bold);
    detected_ = new QTreeWidget;
    detected_->setRootIsDecorated(false);
    detected_->setColumnCount(4);
    detected_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    dl->addWidget(detectedTitle_);
    dl->addWidget(detected_, 1);
    bh->addLayout(dl, 2);
    auto *tl = new QVBoxLayout;
    tipsTitle_ = new QLabel;
    tipsTitle_->setFont(bold);
    tips_ = new QTextBrowser;
    tips_->setObjectName(QStringLiteral("tips"));
    tips_->setOpenExternalLinks(true);
    tl->addWidget(tipsTitle_);
    tl->addWidget(tips_, 1);
    bh->addLayout(tl, 3);
    split->addWidget(bottom);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    connect(startBtn_, &QPushButton::clicked, this, &ElrsAnalyzer::toggle);
    connect(use_, &QComboBox::currentIndexChanged, this, [this] {
        appSettings().setValue("elrs/use", use_->currentIndex());
        run_.candVerdict = elrs::Verdict::Unknown;  // al momento, sin esperar
        run_.tipsT = -1e9;
        recompute();
    });
    connect(pilots_, &QSpinBox::valueChanged, this, [this](int n) {
        appSettings().setValue("elrs/pilots", n);
        run_.candVerdict = elrs::Verdict::Unknown;  // al momento, sin esperar
        run_.tipsT = -1e9;
        recompute();
    });
    connect(region_, &QComboBox::currentIndexChanged, this, [this] {
        appSettings().setValue("elrs/region", region_->currentIndex());
        run_.candVerdict = elrs::Verdict::Unknown;  // al momento, sin esperar
        run_.tipsT = -1e9;
        recompute();
    });
    connect(autoStop_, &QCheckBox::toggled, this, [](bool on) { appSettings().setValue("elrs/autoStop", on); });
    connect(memory_, &QComboBox::currentIndexChanged, this, [this] {
        const int n = memory_->currentData().toInt();
        appSettings().setValue("elrs/memory", n);
        if (session_.running()) emit requestMemory(n);
        updateAutoStopTip();  // dice cuantos barridos
    });

    connect(this, &ElrsAnalyzer::runningChanged, view_, &ElrsView::setScanning);
    connect(this, &ElrsAnalyzer::requestStart, backend, &Backend::startSurvey);
    connect(this, &ElrsAnalyzer::requestStop, backend, &Backend::stopSurvey);
    connect(this, &ElrsAnalyzer::requestMemory, backend, &Backend::setSurveyMemory);
    connect(backend, &Backend::surveyUpdated, this, &ElrsAnalyzer::onSurvey);
    connect(backend, &Backend::surveyStopped, this, &ElrsAnalyzer::onStopped);
    connect(backend, &Backend::statusChanged, this, [this](const RadioStatus &s) { setAdapterOpen(s.open); });

    retranslate();
    state_->setText(tr("Connect the adapter first."));
    resize(1000, 700);
    recompute();
}

void ElrsAnalyzer::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) retranslate();
    QDialog::changeEvent(e);
}

void ElrsAnalyzer::closeEvent(QCloseEvent *e) {
    stop();
    QDialog::closeEvent(e);
}

void ElrsAnalyzer::retranslate() {
    setWindowTitle(tr("2.4 GHz analyzer (ELRS)"));
    useLbl_->setText(tr("I fly"));
    use_->setItemText(0, tr("Racing"));
    use_->setItemText(1, tr("Freestyle"));
    use_->setItemText(2, tr("Long range"));
    use_->setToolTip(tr("What you fly: it changes which packet rates are suggested"));
    regionLbl_->setText(tr("ELRS regulatory domain"));
    pilotsLbl_->setText(tr("Other ELRS pilots"));
    pilots_->setToolTip(tr("How many other pilots will fly ELRS 2.4 GHz at the same time as you (not counting you). "
                           "Their packets hop over the same 80 channels and sometimes meet yours. Pilots already "
                           "flying while you measure also show in the spectrum as short bursts."));
    region_->setItemText(0, tr("ISM 2.4 GHz (FCC, most countries)"));
    region_->setItemText(1, tr("EU LBT (CE_LBT)"));
    region_->setToolTip(tr("The 2.4 GHz regulatory domain your ELRS firmware was built with (shown as ISM2G4 or "
                           "CE_LBT). The EU build listens before transmitting above 10 mW and skips the packet when "
                           "the channel is busy."));
    memoryLbl_->setText(tr("Average"));
    for (int i = 0; i < memory_->count(); i++)
        memory_->setItemText(i, tr("last %n sweep(s)", nullptr, memory_->itemData(i).toInt()));
    memory_->setToolTip(tr("How many recent sweeps the graph and the advice average. Fewer: it follows changes "
                           "sooner; more: it also catches WiFi that only transmits now and then."));
    startBtn_->setText(session_.running() ? tr("Stop") : tr("Start analysis"));
    autoStop_->setText(tr("Stop when the result is steady"));
    updateAutoStopTip();
    detectedTitle_->setText(tr("On the air"));
    tipsTitle_->setText(tr("How to set up ELRS here"));
    detected_->setHeaderLabels({tr("Signal"), tr("Frequency"), tr("Peak"), tr("Activity")});
    buildLegend();
    run_.tipsT = -1e9;
    run_.tipsHtml.clear();
    run_.shownKeys.clear();  // "En el aire": los textos, otra vez
    if (!session_.running() && session_.adapterOpen()) state_->setText(tr("Ready: press Start analysis."));
    else if (!session_.adapterOpen()) state_->setText(tr("Connect the adapter first."));
    recompute();
}

void ElrsAnalyzer::updateAutoStopTip() {
    autoStop_->setToolTip(tr("Stop the analysis by itself once the average holds %n sweeps, it has run for at least "
                             "%1 s and the verdict and the suggestions have not changed for %2 s; and after %3 s "
                             "in any case.",
                             nullptr, memory_->currentData().toInt())
                              .arg(int(kMinRunS))
                              .arg(int(kSteadyS))
                              .arg(int(kMaxRunS)));
}

void ElrsAnalyzer::buildLegend() {
    if (!legendGraph_ || legendText_.size() < 9) return;
    legendGraph_->setText(tr("Graph:"));
    legendElrs_->setText(tr("ELRS channels:"));
    const QString t[] = {tr("average level"), tr("peaks"),        tr("bursts"),  tr("WiFi network"), tr("sweep"),
                         tr("clean"),         tr("busy at times"), tr("busy often"), tr("not measured")};
    for (int i = 0; i < 9; i++) legendText_[i]->setText(t[i] + QStringLiteral("   "));
    legendNote_->setText(tr("dB over the adapter's noise floor, measured where the adapter is (the drone's receiver in "
                            "flight may hear more or less). The adapter does not reach below 2402 MHz: the two lowest "
                            "ELRS channels (2400.4 and 2401.4) are not measured, and near the band edges signals read "
                            "a few dB low."));
}


void ElrsAnalyzer::setAdapterOpen(bool open) {
    bool lostRun;
    if (!session_.setAdapterOpen(open, &lostRun)) return;
    startBtn_->setEnabled(open);
    if (lostRun) {
        startBtn_->setText(tr("Start analysis"));
        emit runningChanged(false);
    }
    if (!session_.running())
        state_->setText(open ? tr("Ready: press Start analysis.") : tr("Connect the adapter first."));
}

void ElrsAnalyzer::toggle() {
    if (session_.running()) {
        stop();
        return;
    }
    run_ = Run{};
    session_.start();
    startBtn_->setText(tr("Stop"));
    state_->setText(tr("Analysing %1-%2 MHz...").arg(kSurveyLo, 0, 'f', 0).arg(kSurveyHi, 0, 'f', 0));
    // primero runningChanged: la ventana principal deja al otro analizador (abandon) antes de pedir este barrido
    emit runningChanged(true);
    emit requestMemory(memory_->currentData().toInt());
    emit requestStart(kSurveyLo, kSurveyHi);
    recompute();
}

void ElrsAnalyzer::stop() {
    if (!session_.stop()) return;
    showStopped(run_.last.sweeps > 0 ? tr("Stopped after %n sweep(s).", nullptr, run_.last.sweeps)
                                 : tr("Ready: press Start analysis."));
    emit requestStop();  // su surveyStopped llega en cola: SurveySession lo descuenta
    emit runningChanged(false);
}

void ElrsAnalyzer::showStopped(const QString &state) {
    startBtn_->setText(tr("Start analysis"));
    state_->setText(state);
}

void ElrsAnalyzer::abandon(const QString &why) {
    if (!session_.abandon()) return;
    showStopped(why);
    emit runningChanged(false);
}

void ElrsAnalyzer::onStopped() {
    if (!session_.onStopped()) return;
    showStopped(tr("Stopped: the receiver went back to scanning or watching."));
    emit runningChanged(false);
}

void ElrsAnalyzer::onSurvey(const SurveyData &d) {
    if (!session_.running()) return;
    // el barrido de 5,8 GHz (de antes de empezar este, o del otro analizador): no es de aqui
    if (d.bins.isEmpty() || d.bins.front().mhz > 3000) return;
    session_.onData();
    run_.last = d;
    run_.sweeps = d.sweeps;
    state_->setText(tr("Analysing %1-%2 MHz: %n sweep(s) so far.", nullptr, d.sweeps)
                        .arg(kSurveyLo, 0, 'f', 0)
                        .arg(kSurveyHi, 0, 'f', 0));
    recompute();
}

elrs::Use ElrsAnalyzer::use() const { return static_cast<elrs::Use>(use_->currentIndex()); }
elrs::Region ElrsAnalyzer::region() const { return static_cast<elrs::Region>(region_->currentIndex()); }

void ElrsAnalyzer::recompute() {
    if (!summary_) return;
    run_.report = elrs::analyse(toBins(run_.last.bins), use(), region(), pilots_->value());
    const bool immediate = run_.candVerdict == elrs::Verdict::Unknown;  // empezar o un ajuste cambiado
    settleVerdict(immediate);
    // las cifras del resumen y la tira de canales, de un informe que dice lo mismo que el veredicto mostrado (si no,
    // durante su espera se contradecian: "banda limpia ... 12 muy ocupados") y como mucho una vez por barrido
    if (run_.shownVerdict == elrs::Verdict::Unknown ||
        (run_.report.verdict == run_.shownVerdict && (immediate || run_.sweeps != run_.shownSweep))) {
        run_.shownReport = run_.report;
        run_.shownSweep = run_.sweeps;
    }
    view_->setData(run_.last.bins, run_.shownReport);  // el espectro, al momento; WiFi y canales, del informe mostrado
    const elrs::Report &r = run_.shownReport;
    if (r.measured == 0) {
        summary_->setText(session_.running() ? tr("Measuring...") : QString());
    } else {
        summary_->setText(verdictText() + QStringLiteral(" — ") +
                          tr("%1 of 80 ELRS channels measured: %2 clean, %3 busy at times, %4 busy often. About %5 "
                             "of the hops land on a busy moment.")
                              .arg(r.measured)
                              .arg(r.clean)
                              .arg(r.busy)
                              .arg(r.bad)
                              .arg(pct(r.hit)) +
                          (r.others > 0
                               ? QStringLiteral(" ") + tr("Other pilots would meet about %1 more.", nullptr,
                                                          r.others).arg(pct(r.pilot_hit))
                               : QString()));
    }
    updateDetected();
    updateTips();
    checkSteady();
}

namespace {
// para el registro: las sugerencias por su nombre (elrs::Tip)
const char *const kTipNames[] = {"RateClean", "RateModerate", "RateHeavy", "Gemini",      "Pilots",
                                 "DynamicPower", "LbtBusy",  "OwnWifi",   "WifiOnTx",    "Hoppers",
                                 "Continuous", "Strong",     "Antennas",  "Telemetry",   "Band900"};
const char *const kVerdictNames[] = {"?", "limpia", "algo", "mucha"};

// clave de updateTips: "veredicto|uso|pilotos,tip,tip..."
struct TipsKey {
    int verdict = 0;
    QStringList rest;
    QSet<int> tips;
};
TipsKey parseKey(const QString &k) {
    TipsKey o;
    const QStringList parts = k.split(QLatin1Char(','));
    const QStringList head = parts.value(0).split(QLatin1Char('|'));
    o.verdict = head.value(0).toInt();
    o.rest = head.mid(1);
    for (int i = 1; i < parts.size(); i++) o.tips.insert(parts[i].toInt());
    return o;
}
}  // namespace

void ElrsAnalyzer::checkSteady() {
    if (!session_.running() || run_.shownVerdict == elrs::Verdict::Unknown) return;
    // lo que importa para configurar la emisora: el veredicto y que sugerencias salen (no sus cifras, ni cada fila de
    // "En el aire": una WiFi debil en el limite de la deteccion iba y venia y no dejaba parar nunca)
    const QString key = run_.currentTipsKey;
    const double t = session_.now();
    if (key != run_.steadyKey) {
        if (!run_.steadyKey.isEmpty()) {
            const TipsKey a = parseKey(run_.steadyKey), b = parseKey(key);
            QStringList what;
            if (a.verdict != b.verdict && a.verdict >= 0 && a.verdict < 4 && b.verdict >= 0 && b.verdict < 4)
                what << QStringLiteral("veredicto %1 -> %2").arg(QLatin1String(kVerdictNames[a.verdict]),
                                                                QLatin1String(kVerdictNames[b.verdict]));
            for (int x : b.tips)
                if (!a.tips.contains(x) && x >= 0 && x < int(std::size(kTipNames))) what << QStringLiteral("+%1").arg(QLatin1String(kTipNames[x]));
            for (int x : a.tips)
                if (!b.tips.contains(x) && x >= 0 && x < int(std::size(kTipNames))) what << QStringLiteral("-%1").arg(QLatin1String(kTipNames[x]));
            if (a.rest != b.rest) what << QStringLiteral("ajustes");
            KP_LOGI("elrs: cambia la recomendacion a los %.0f s (barrido %d): %s", t, run_.sweeps,
                    qUtf8Printable(what.join(QLatin1Char(' '))));
            run_.lastChange = a.verdict != b.verdict ? tr("the verdict") : tr("which suggestions apply");
        }
        run_.steadyKey = key;
        run_.steadySince = t;
    }
    if (!autoStop_->isChecked()) return;
    // la media ya tiene sus barridos y ha corrido un minimo (un AP callado se pilla a ratos)
    const int memory = memory_->currentData().toInt();
    if (run_.sweeps < memory || t < kMinRunS) return;
    const int sweeps = run_.sweeps;
    if (t - run_.steadySince >= kSteadyS) {
        KP_LOGI("elrs: parado solo a los %.0f s (barrido %d): %.0f s sin cambios", t, sweeps, t - run_.steadySince);
        stop();
        state_->setText(tr("Stopped by itself after %n sweep(s): the suggestions had not changed for %1 s. Start "
                           "again to measure anew.", nullptr, sweeps)
                            .arg(int(kSteadyS)));
    } else if (t >= kMaxRunS) {
        // la banda no se queda quieta (WiFi que van y vienen, una emisora que se enciende): con esto ya hay bastante
        KP_LOGI("elrs: parado solo a los %.0f s (barrido %d): sigue cambiando", t, sweeps);
        stop();
        state_->setText(tr("Stopped by itself after %1 s: the band keeps changing (last change: %2), so the suggestions "
                           "cover what was seen in that time. Start again to measure anew.")
                            .arg(int(kMaxRunS))
                            .arg(run_.lastChange));
    }
}

void ElrsAnalyzer::settleVerdict(bool immediate) {
    const elrs::Verdict v = run_.report.verdict;
    const double t = session_.now();
    if (v == elrs::Verdict::Unknown) {  // aun sin medir lo bastante (al empezar): nada que mostrar
        run_.shownVerdict = run_.candVerdict = v;
        return;
    }
    if (immediate || run_.shownVerdict == elrs::Verdict::Unknown) {
        run_.shownVerdict = run_.candVerdict = v;
        run_.candSince = t;
        return;
    }
    if (v == run_.shownVerdict) {
        run_.candVerdict = v;
        run_.candSince = t;
    } else if (v != run_.candVerdict) {
        run_.candVerdict = v;
        run_.candSince = t;
    } else if (t - run_.candSince >= kHoldS) {
        run_.shownVerdict = v;
    }
}

QString ElrsAnalyzer::verdictText() const {
    switch (run_.shownVerdict) {
    case elrs::Verdict::Clean: return tr("Clean band");
    case elrs::Verdict::Moderate: return tr("Some interference");
    case elrs::Verdict::Heavy: return tr("Heavy interference");
    case elrs::Verdict::Unknown: break;
    }
    return tr("Still measuring");
}

void ElrsAnalyzer::updateDetected() {
    const double t = session_.now();
    const auto put = [&](const QString &key, const QStringList &cols, const QColor &colour) {
        run_.held[key] = Held{cols, colour, t};
    };
    for (const auto &w : run_.report.wifi)
        put(QStringLiteral("a:%1").arg(w.ch, 2, 10, QLatin1Char('0')),
            {tr("WiFi channel %1").arg(w.ch), QStringLiteral("%1 MHz").arg(w.mhz, 0, 'f', 0),
             QStringLiteral("%1 dB").arg(w.peak_db, 0, 'f', 0),
             w.traffic ? tr("traffic, %1 of the time").arg(pct(w.duty)) : tr("beacons only (idle)")},
            kWifiColour);
    for (const auto &e : run_.report.other) {
        const QString pk = QStringLiteral("%1 dB").arg(e.peak_db, 0, 'f', 0);
        switch (e.kind) {
        case elrs::Other::Hoppers:
            put(QStringLiteral("b:hop"),
                {tr("Hopping signals (ELRS, Bluetooth, other radios)"), tr("whole band"), pk,
                 tr("short bursts in %1 of the channels").arg(pct(e.share))},
                QColor());
            break;
        case elrs::Other::Continuous:
            // por MHz redondeados a 5: la misma senal no cambia de fila por medio MHz
            put(QStringLiteral("c:%1").arg(int(std::lround((e.lo_mhz + e.hi_mhz) / 10)) * 5),
                {tr("Continuous signal"), QStringLiteral("%1-%2 MHz").arg(e.lo_mhz, 0, 'f', 1).arg(e.hi_mhz, 0, 'f', 1),
                 pk, tr("always on")},
                QColor());
            break;
        case elrs::Other::Strong:
            put(QStringLiteral("d:strong"),
                {tr("Very strong signal nearby"), QStringLiteral("%1 MHz").arg((e.lo_mhz + e.hi_mhz) / 2, 0, 'f', 1), pk,
                 QString()},
                QColor());
            break;
        }
    }
    // lo que lleva un rato sin verse, fuera (al parar, se queda lo ultimo)
    if (session_.running())
        for (auto it = run_.held.begin(); it != run_.held.end();)
            it = t - it->seen > kKeepS ? run_.held.erase(it) : std::next(it);
    QStringList keys = run_.held.keys();
    if (keys.isEmpty() && run_.report.measured > 0) keys << QStringLiteral("z:none");
    if (keys != run_.shownKeys) {  // otras filas: rehacer la lista (conservando el desplazamiento)
        if (session_.running())
            KP_LOGI("elrs: en el aire a los %.0f s: %s", t, qUtf8Printable(keys.join(QLatin1Char(' '))));
        const int scroll = detected_->verticalScrollBar()->value();
        detected_->clear();
        for (const auto &k : keys) {
            auto *it = new QTreeWidgetItem(detected_);
            if (k == QLatin1String("z:none")) it->setText(0, tr("Nothing besides noise"));
        }
        detected_->verticalScrollBar()->setValue(scroll);
        run_.shownKeys = keys;
    }
    for (int r = 0; r < keys.size(); r++) {  // y los textos, en su sitio
        const auto it = run_.held.constFind(keys[r]);
        if (it == run_.held.constEnd()) continue;
        QTreeWidgetItem *item = detected_->topLevelItem(r);
        for (int c = 0; c < it->cols.size(); c++)
            if (item->text(c) != it->cols[c]) {
                item->setText(c, it->cols[c]);
                item->setToolTip(c, it->cols[c]);  // una columna estrecha corta el texto
            }
        if (it->colour.isValid()) item->setForeground(0, it->colour);
        // visto hace un rato (no en este barrido): mas tenue
        const bool stale = t - it->seen > kDimS;
        for (int c = 0; c < 4; c++) {
            QColor fg = c == 0 && it->colour.isValid() ? it->colour : detected_->palette().color(QPalette::Text);
            if (stale) fg.setAlpha(120);
            item->setForeground(c, fg);
        }
    }
}

QString ElrsAnalyzer::rateAdvice(elrs::Verdict v) const {
    switch (use()) {
    case elrs::Use::Racing:
        if (v == elrs::Verdict::Clean)
            return tr("<b>Packet rate:</b> the band is clean: <b>F1000</b> or <b>F500</b> (FLRC, lowest latency) or "
                      "<b>500 Hz</b> (LoRa). The ELRS documentation recommends 500 Hz or faster for racing.");
        if (v == elrs::Verdict::Moderate)
            return tr("<b>Packet rate:</b> <b>500 Hz</b> (LoRa: the ELRS documentation notes that 2.4 GHz LoRa "
                      "handles WiFi noise very well) or <b>D500</b>, which sends every packet twice on 2 different "
                      "frequencies (higher LQ, a little more latency).");
        return tr("<b>Packet rate:</b> <b>D250</b> (every packet 4 times across 3 frequencies) or <b>D500</b> (twice "
                  "on 2 frequencies): higher LQ, a little more latency. Avoid F1000/F500 without repeats.");
    case elrs::Use::Freestyle:
        if (v == elrs::Verdict::Clean)
            return tr("<b>Packet rate:</b> <b>250 Hz</b> or <b>500 Hz</b> (LoRa): good range and response. F500 also "
                      "works in a clean band.");
        if (v == elrs::Verdict::Moderate)
            return tr("<b>Packet rate:</b> <b>250 Hz</b> (LoRa, more sensitive) or <b>D500</b> (every packet twice on "
                      "2 different frequencies). Avoid F1000.");
        return tr("<b>Packet rate:</b> <b>D250</b> (every packet 4 times across 3 frequencies) or <b>150 Hz</b> / "
                  "<b>100 Hz Full</b> (LoRa, more sensitive).");
    case elrs::Use::LongRange:
        if (v == elrs::Verdict::Clean)
            return tr("<b>Packet rate:</b> <b>150 Hz</b> or <b>100 Hz Full</b> for range with good response; "
                      "<b>50 Hz</b> for the most range (the best sensitivity).");
        if (v == elrs::Verdict::Moderate)
            return tr("<b>Packet rate:</b> <b>50 Hz</b> or <b>100 Hz Full</b> (LoRa, the most sensitive): the link "
                      "margin is what keeps you flying through the interference.");
        return tr("<b>Packet rate:</b> <b>50 Hz</b>. For long range here, consider ELRS on 900 MHz instead.");
    }
    return {};
}

QString ElrsAnalyzer::modeTable() const {
    QString rows;
    for (const auto id : run_.report.modes) {
        const elrs::Mode &m = elrs::mode(id);
        rows += QStringLiteral("<tr><td>%1</td><td>%2</td><td align='right'>%3 dBm</td><td align='right'>%4</td></tr>")
                    .arg(QString::fromLatin1(m.name))
                    .arg(m.flrc ? QStringLiteral("FLRC") : QStringLiteral("LoRa"))
                    .arg(m.sensitivity)
                    .arg(pct(elrs::packet_loss(m, run_.report.hit, run_.report.others)));
    }
    const QString note =
        run_.report.others > 0
            ? tr("Estimate from what was measured and %n other pilot(s) flying the same mode. A packet that meets "
                 "another pilot's is often still received if it arrives stronger: take it as an upper bound.",
                 nullptr, run_.report.others)
            : tr("Estimate from what was measured: with repeats (D250, D500) a packet is lost only if every "
                 "frequency it goes out on is busy.");
    return QStringLiteral("<table cellspacing='0' cellpadding='3' style='margin-left:12px'><tr><th align='left'>%1</th>"
                          "<th align='left'>%2</th><th align='right'>%3</th><th align='right'>%4</th></tr>%5</table>"
                          "<p style='margin-left:12px'><small>%6</small></p>")
        .arg(tr("Mode"), tr("Modulation"), tr("Sensitivity"), tr("Packets lost (estimate)"), rows, note);
}

QString ElrsAnalyzer::tipText(elrs::Tip t) const {
    using elrs::Tip;
    switch (t) {
    case Tip::RateClean: return rateAdvice(elrs::Verdict::Clean);
    case Tip::RateModerate: return rateAdvice(elrs::Verdict::Moderate);
    case Tip::RateHeavy: return rateAdvice(elrs::Verdict::Heavy);
    case Tip::Gemini:
        return tr("<b>Gemini:</b> with a Gemini TX module and a true-diversity receiver, every packet goes out at once "
                  "on two frequencies 40 MHz apart: a hop that lands on a WiFi burst or on another pilot's packet is "
                  "not lost. It raises LQ, not range. The ELRS documentation recommends it for events with 6-8 "
                  "pilots. With a single-antenna receiver, leave the TX in Switch mode.");
    case Tip::Pilots:
        return tr("<b>%n other pilot(s):</b> about %1 of your packets would meet one of theirs on the same channel at "
                  "the same moment (all on 500 Hz; less with F500, which is on the air for less time). Use your own "
                  "binding phrase, never a common one: it sets your hop sequence. Turn off the radios of pilots who "
                  "are not flying: a handset left on in the pits still transmits.",
                  nullptr, run_.report.others)
            .arg(pct(run_.report.pilot_hit));
    case Tip::DynamicPower:
        return tr("<b>Dynamic Power:</b> set Max Power with some headroom (within your legal limit) and Dynamic to "
                  "Dyn: the TX lowers power while the link is good and jumps to the maximum when LQ drops. It needs "
                  "telemetry (not with Telem Ratio Off or Race); with an AUX switch, switch low forces Max Power.");
    case Tip::LbtBusy:
        return tr("<b>EU LBT (CE_LBT):</b> above 10 mW the radios listen before transmitting and skip the packet "
                  "when the channel is busy (EN 300 328). With this much activity more packets are skipped: fly "
                  "closer, or remove the sources nearby.");
    case Tip::OwnWifi: {
        std::vector<int> nums;
        for (const auto &w : run_.report.wifi)
            if (w.peak_db >= elrs::kWifiStrongDb || w.traffic) nums.push_back(w.ch);
        std::sort(nums.begin(), nums.end());
        QStringList chs;
        for (int n : nums) chs << QString::number(n);
        return tr("<b>WiFi:</b> strong network on channel %1 (%n network(s)). If it is yours (router, phone hotspot, "
                  "goggles' or DVR WiFi, a laptop), turn its 2.4 GHz off or move it to 5 GHz. If not, move away from "
                  "it: distance helps more than any setting. ELRS hops over the whole band, so a WiFi channel cannot "
                  "be avoided by choosing ELRS channels.",
                  nullptr, int(nums.size()))
            .arg(chs.join(QStringLiteral(", ")));
    }
    case Tip::WifiOnTx:
        return tr("<b>ELRS's own WiFi:</b> the TX module (Enable WiFi), the VRx backpack (Enable VRX WiFi) and the "
                  "receiver use 2.4 GHz WiFi for updates. The receiver turns its WiFi on by itself if it gets no link "
                  "for a while (60 s with the default options): power the receiver with the TX already on, and do "
                  "not leave any of them in WiFi mode while flying.");
    case Tip::Hoppers:
        return tr("<b>Other radios:</b> short bursts all over the band: Bluetooth, other radio links or other pilots' "
                  "ELRS. ELRS hops past them; make sure every pilot uses their own binding phrase. If it is your own "
                  "radio, turn it and the receiver off while measuring: your own link is not interference, but it "
                  "hides what else is on the air (it hid two WiFi networks in a test).");
    case Tip::Continuous:
        for (const auto &e : run_.report.other)
            if (e.kind == elrs::Other::Continuous)
                return tr("<b>Continuous signal</b> at %1-%2 MHz (not WiFi): it could be a 2.4 GHz analog video "
                          "transmitter, a wireless camera or a baby monitor. ELRS loses every hop that lands there: "
                          "find it and turn it off, or move away from it.")
                    .arg(e.lo_mhz, 0, 'f', 0)
                    .arg(e.hi_mhz, 0, 'f', 0);
        return {};
    case Tip::Strong:
        for (const auto &e : run_.report.other)
            if (e.kind == elrs::Other::Strong)
                return tr("<b>Very strong signal</b> (%1 dB over the noise) at %2 MHz: something transmits very close "
                          "to the adapter. A transmitter that close can deafen a receiver even outside its own "
                          "channel: keep the receiver's antennas away from the VTX and other radios, and the TX module "
                          "away from phones and routers.")
                    .arg(e.peak_db, 0, 'f', 0)
                    .arg((e.lo_mhz + e.hi_mhz) / 2, 0, 'f', 0);
        return {};
    case Tip::Antennas:
        return tr("<b>Antennas</b> (common practice, not from the ELRS documentation): the receiver's at 90° to each "
                  "other, out of the carbon frame and away from the VTX antenna and the power wires; the TX antenna "
                  "not hidden behind your body.");
    case Tip::Telemetry:
        if (use() == elrs::Use::Racing)
            return tr("<b>Telemetry ratio:</b> the ELRS documentation recommends <b>Race</b> for racing: while armed, "
                      "every packet goes to your sticks (without telemetry there is no Dynamic Power).");
        return tr("<b>Telemetry ratio:</b> <b>Std</b> (it follows the packet rate). Every telemetry packet is one "
                  "your sticks do not get (1:64 = one in 64), so do not raise it in a busy band.");
    case Tip::Band900:
        return tr("<b>Another band:</b> 2.4 GHz is very busy here. If you always fly at this spot, ELRS on 900 MHz "
                  "(868/915) does not share the band with WiFi; with LR1121 hardware, GemX sends on 900 MHz and "
                  "2.4 GHz at once.");
    }
    return {};
}

void ElrsAnalyzer::updateTips() {
    const double t = session_.now();
    QString html, key;
    if (run_.shownVerdict == elrs::Verdict::Unknown) {
        key = session_.running() ? QStringLiteral("wait") : QStringLiteral("idle");
        html = QStringLiteral("<p>%1</p>")
                   .arg(session_.running() ? tr("Wait for a few sweeps: the suggestions need most of the band measured.")
                                 : tr("Press Start analysis. In a couple of sweeps (a few seconds) you will see how busy "
                                      "each ELRS channel is and how to set up ELRS for it."));
    } else {
        // las del veredicto que se muestra; las demas (WiFi fuerte, emision continua...) siguen kKeepS tras dejar de salir
        elrs::Report shown = run_.report;
        shown.verdict = run_.shownVerdict;
        const std::vector<elrs::Tip> now = elrs::tips_for(shown, region());
        for (const auto tip : now) run_.tipSeen[int(tip)] = t;
        const auto isRate = [](elrs::Tip x) {
            return x == elrs::Tip::RateClean || x == elrs::Tip::RateModerate || x == elrs::Tip::RateHeavy;
        };
        std::vector<elrs::Tip> tips;
        // midiendo, lo que lleva kKeepS sin salir se olvida; parado, se queda lo que habia al parar
        if (session_.running())
            for (auto it = run_.tipSeen.begin(); it != run_.tipSeen.end();)
                it = t - it.value() > kKeepS ? run_.tipSeen.erase(it) : std::next(it);
        for (auto it = run_.tipSeen.cbegin(); it != run_.tipSeen.cend(); ++it) {
            const auto tip = static_cast<elrs::Tip>(it.key());
            const bool current = std::find(now.begin(), now.end(), tip) != now.end();
            if (current || !isRate(tip)) tips.push_back(tip);
        }
        std::sort(tips.begin(), tips.end(), [](elrs::Tip a, elrs::Tip b) { return elrs::tip_order(a) < elrs::tip_order(b); });
        key = QString::number(int(run_.shownVerdict)) + QLatin1Char('|') + QString::number(int(use())) + QLatin1Char('|') +
              QString::number(run_.report.others);
        for (const auto tip : tips) key += QLatin1Char(',') + QString::number(int(tip));
        html = QStringLiteral("<p>%1</p><ul>").arg(
            tr("ELRS hops over all 80 channels in an order set by the binding phrase, and no channel can be left out: "
               "what you can choose is how robust each packet is, the power and where the antennas go."));
        for (const auto tip : tips) {
            const QString text = tipText(tip);
            if (text.isEmpty()) continue;
            html += QStringLiteral("<li style='margin-bottom:6px'>%1%2</li>").arg(text, isRate(tip) ? modeTable() : QString());
        }
        html += QStringLiteral("</ul><p><small>%1 <a href='https://www.expresslrs.org/info/signal-health/'>Signal health</a>"
                               " · <a href='https://www.expresslrs.org/quick-start/transmitters/lua-howto/'>Lua</a>"
                               " · <a href='https://www.expresslrs.org/software/gemini/'>Gemini</a>"
                               " · <a href='https://www.expresslrs.org/software/dynamic-transmit-power/'>Dynamic Power</a>"
                               "</small></p>")
                    .arg(tr("From the ExpressLRS documentation:"));
    }
    run_.currentTipsKey = key;
    // mismas sugerencias: sus cifras se rehacen como mucho cada kRenderS; nunca se mueve lo que se esta leyendo
    if (html == run_.tipsHtml) return;
    if (key == run_.tipsKey && t - run_.tipsT < kRenderS) return;
    const int scroll = tips_->verticalScrollBar()->value();
    tips_->setHtml(html);
    tips_->verticalScrollBar()->setValue(scroll);
    run_.tipsHtml = html;
    run_.tipsKey = key;
    run_.tipsT = t;
}

