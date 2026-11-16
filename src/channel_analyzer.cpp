#include "channel_analyzer.h"
#include "log.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QElapsedTimer>
#include <QSignalBlocker>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>

#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
const char kBands[] = "ABEFRL";
constexpr double kMargin = 15;  // MHz analizados a cada lado de los canales elegidos (HDZero: 13.5)

QColor ratingColour(const advisor::Rating &r) {
    if (r.busy >= 0 || r.cost >= 8) return QColor(0xd9, 0x48, 0x3b);
    if (r.cost >= 3) return QColor(0xe0, 0xa0, 0x20);
    return QColor(0x3a, 0xa8, 0x5a);
}

// el mismo fichero que MainWindow::settings() (sin depender de la ventana principal: test/analyzer_shot.cpp)
QSettings &settings() {
    static QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("FPVDesktopWiFiReceiver"),
                       QStringLiteral("FPVDesktopWiFiReceiver"));
    return s;
}
}  // namespace

// ---- grafica ------------------------------------------------------------------------------------------------

SpectrumView::SpectrumView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SpectrumView::setData(const SurveyData &d, const advisor::Advice &a, double lo, double hi) {
    data_ = d;
    advice_ = a;
    lo_ = lo;
    hi_ = hi;
    update();
}

void SpectrumView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.base());
    const QFontMetrics fm(font());
    const int rowH = fm.height() + 4;
    const int left = std::max({fm.horizontalAdvance(QStringLiteral("40 dB")), fm.horizontalAdvance(tr("Analog")),
                               fm.horizontalAdvance(QStringLiteral("HDZero"))}) + 10,
              right = width() - 8, top = 8;
    const int plotBottom = height() - 2 * rowH - fm.height() - 10;
    const double dbMax = 40;
    const auto X = [&](double mhz) { return left + (mhz - lo_) / (hi_ - lo_) * (right - left); };
    const auto Y = [&](double db) { return plotBottom - std::clamp(db, 0.0, dbMax) / dbMax * (plotBottom - top); };

    // rejilla y ejes
    p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DotLine));
    for (int db = 10; db <= 40; db += 10) {
        p.drawLine(QPointF(left, Y(db)), QPointF(right, Y(db)));
        p.drawText(QRectF(0, Y(db) - fm.height() / 2.0, left - 4, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 dB").arg(db));
    }
    p.setPen(pal.color(QPalette::Text));
    for (int f = int(std::ceil(lo_ / 50) * 50); f <= hi_; f += 50) {
        p.drawLine(QPointF(X(f), plotBottom), QPointF(X(f), plotBottom + 3));
        p.drawText(QRectF(X(f) - 30, plotBottom + 3, 60, fm.height()), Qt::AlignCenter, QString::number(f));
    }
    p.drawLine(QPointF(left, plotBottom), QPointF(right, plotBottom));

    // recomendados: franja con su ancho
    for (const auto &pk : advice_.picks) {
        const double half = advisor::width(pk.r.ch) / 2;
        QColor c = pk.r.ch.hdzero ? QColor(0x7b, 0x5c, 0xd6) : QColor(0x2f, 0x80, 0xed);
        c.setAlpha(45);
        p.fillRect(QRectF(QPointF(X(pk.r.ch.mhz - half), top), QPointF(X(pk.r.ch.mhz + half), plotBottom)), c);
    }

    // espectro: rafagas (fondo naranja), picos (linea fina) y media (area)
    if (!data_.bins.isEmpty()) {
        for (const auto &b : data_.bins) {
            if (b.mhz < lo_ || b.mhz > hi_ || !(b.peakDb > 10 && b.peakDb - b.meanDb > 10)) continue;
            p.fillRect(QRectF(QPointF(X(b.mhz - 0.08), top), QPointF(X(b.mhz + 0.08), plotBottom)), QColor(255, 140, 0, 40));
        }
        QPainterPath peak, mean;
        bool first = true;
        double lastX = 0;
        for (const auto &b : data_.bins) {
            if (b.mhz < lo_ || b.mhz > hi_) continue;
            const double x = X(b.mhz);
            if (first || x - lastX > 4) {  // hueco (sin datos): empezar otro tramo
                peak.moveTo(x, Y(b.peakDb));
                mean.moveTo(x, plotBottom);
            } else {
                peak.lineTo(x, Y(b.peakDb));
            }
            mean.lineTo(x, Y(b.meanDb));
            first = false;
            lastX = x;
        }
        p.setPen(QPen(QColor(0xe0, 0x70, 0x30), 1));
        p.drawPath(peak);
        QColor fill = pal.color(QPalette::Highlight);
        p.setPen(QPen(fill, 1.5));
        fill.setAlpha(90);
        p.setBrush(fill);
        p.drawPath(mean);
        p.setBrush(Qt::NoBrush);
    }

    // lo que hay en el aire
    for (const auto &s : data_.pilots) {
        if (s.ageS >= 60 || s.freqMHz < lo_ || s.freqMHz > hi_) continue;
        const QString t = (s.name.isEmpty() ? QString::number(s.freqMHz, 'f', 0) : s.name) +
                          (s.isDigital() ? QStringLiteral(" HDZ") : QString());
        p.setPen(QPen(QColor(0xd9, 0x48, 0x3b), 1.5, Qt::DashLine));
        p.drawLine(QPointF(X(s.freqMHz), top), QPointF(X(s.freqMHz), plotBottom));
        p.setPen(QColor(0xd9, 0x48, 0x3b));
        p.drawText(QPointF(X(s.freqMHz) + 3, top + fm.ascent()), t);
    }

    // filas de canales: analogicos y HDZero, coloreados por interferencia; recomendados en negrita
    const auto row = [&](const std::vector<advisor::Rating> &rs, int y, const QString &title) {
        if (rs.empty()) return;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, y, left - 4, rowH), Qt::AlignRight | Qt::AlignVCenter, title);
        for (const auto &r : rs) {
            if (r.ch.mhz < lo_ || r.ch.mhz > hi_) continue;
            bool picked = false;
            for (const auto &pk : advice_.picks) picked = picked || (pk.r.ch.name == r.ch.name && pk.r.ch.hdzero == r.ch.hdzero);
            const QColor c = ratingColour(r);
            const QRectF box(X(r.ch.mhz) - 2.5, y + 2, 5, rowH - 4);
            p.fillRect(box, c);
            if (picked) {
                QFont bf = font();
                bf.setBold(true);
                p.setFont(bf);
                p.setPen(pal.color(QPalette::Text));
                p.drawText(QRectF(X(r.ch.mhz) - 20, y - fm.height() + 2, 40, fm.height()), Qt::AlignCenter,
                           QString::fromStdString(r.ch.name));
                p.setFont(font());
                p.drawRect(box.adjusted(-1.5, -1.5, 1.5, 1.5));
            }
        }
    };
    const int rowsTop = plotBottom + fm.height() + 6;
    row(advice_.analog, rowsTop, tr("Analog"));
    row(advice_.hdzero, rowsTop + rowH, QStringLiteral("HDZero"));
}

// ---- ventana ------------------------------------------------------------------------------------------------

ChannelAnalyzer::ChannelAnalyzer(Backend *backend, QWidget *parent) : QDialog(parent) {
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    auto *h = new QHBoxLayout(this);

    auto *side = new QVBoxLayout;
    bandsTitle_ = new QLabel;
    bandsTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(bandsTitle_);
    const QString saved = settings().value("analyzer/bands", QStringLiteral("ABEFR")).toString();
    for (const char *b = kBands; *b; b++) {
        auto *c = new QCheckBox;
        c->setChecked(saved.contains(QLatin1Char(*b)));
        connect(c, &QCheckBox::toggled, this, [this] {
            settings().setValue("analyzer/bands", bands());
            startBtn_->setEnabled(open_ && !bands().isEmpty());
            if (running_) {  // otra zona que barrer: se empieza de nuevo
                double lo, hi;
                range(lo, hi);
                if (!bands().isEmpty()) emit requestStart(lo, hi);
                else stop();
            }
            recompute();
        });
        bandBoxes_.push_back(c);
        side->addWidget(c);
    }
    side->addSpacing(8);
    pilotsTitle_ = new QLabel;
    pilotsTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(pilotsTitle_);
    auto *g = new QGridLayout;
    analogLbl_ = new QLabel;
    hdzLbl_ = new QLabel;
    nAnalog_ = new QSpinBox;
    nHdz_ = new QSpinBox;
    nAnalog_->setRange(0, 8);
    nHdz_->setRange(0, 8);
    nAnalog_->setValue(settings().value("analyzer/analog", 4).toInt());
    nHdz_->setValue(settings().value("analyzer/hdzero", 0).toInt());
    for (QSpinBox *s : {nAnalog_, nHdz_})
        connect(s, &QSpinBox::valueChanged, this, [this] {
            settings().setValue("analyzer/analog", nAnalog_->value());
            settings().setValue("analyzer/hdzero", nHdz_->value());
            recompute();
        });
    g->addWidget(analogLbl_, 0, 0);
    g->addWidget(nAnalog_, 0, 1);
    g->addWidget(hdzLbl_, 1, 0);
    g->addWidget(nHdz_, 1, 1);
    narrow_ = new QCheckBox;
    narrow_->setChecked(settings().value("analyzer/narrow", false).toBool());
    connect(narrow_, &QCheckBox::toggled, this, [this](bool on) {
        settings().setValue("analyzer/narrow", on);
        recompute();
    });
    g->addWidget(narrow_, 2, 0, 1, 2);
    side->addLayout(g);
    side->addSpacing(8);
    airTitle_ = new QLabel;
    airTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(airTitle_);
    mine_ = settings().value("analyzer/mine").toStringList();
    air_ = new QListWidget;
    air_->setMaximumHeight(110);
    connect(air_, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        const QString k = it->data(Qt::UserRole).toString();
        mine_.removeAll(k);
        if (it->checkState() == Qt::Checked) mine_ << k;
        settings().setValue("analyzer/mine", mine_);
        recompute();
    });
    side->addWidget(air_);
    side->addSpacing(8);
    startBtn_ = new QPushButton;
    startBtn_->setEnabled(false);
    connect(startBtn_, &QPushButton::clicked, this, &ChannelAnalyzer::toggle);
    side->addWidget(startBtn_);
    state_ = new QLabel;
    state_->setWordWrap(true);
    side->addWidget(state_);
    hint_ = new QLabel;
    hint_->setWordWrap(true);
    side->addWidget(hint_);
    side->addStretch(1);
    auto *sideW = new QWidget;
    sideW->setLayout(side);
    sideW->setFixedWidth(220);
    h->addWidget(sideW);

    auto *main = new QVBoxLayout;
    view_ = new SpectrumView;
    main->addWidget(view_, 3);
    {
        // leyenda: muestra dibujada + texto, en dos columnas (espectro | marcas de canal)
        auto *box = new QWidget;
        auto *lg = new QGridLayout(box);
        lg->setContentsMargins(0, 2, 0, 2);
        lg->setHorizontalSpacing(6);
        lg->setVerticalSpacing(1);
        const QColor hl = palette().color(QPalette::Highlight);
        const auto swatch = [](int kind, QColor c) {
            QPixmap px(22, 12);
            px.fill(Qt::transparent);
            QPainter p(&px);
            if (kind == 0) {  // area: relleno y borde arriba
                QColor f = c;
                f.setAlpha(90);
                p.fillRect(0, 3, 22, 9, f);
                p.setPen(QPen(c, 1.5));
                p.drawLine(0, 3, 22, 3);
            } else if (kind == 1) {  // linea
                p.setPen(QPen(c, 1.5));
                p.drawLine(0, 6, 22, 6);
            } else if (kind == 2) {  // discontinua vertical
                p.setPen(QPen(c, 1.5, Qt::DashLine));
                p.drawLine(11, 0, 11, 12);
            } else if (kind == 3) {  // fondo/franja
                p.fillRect(0, 0, 22, 12, c);
            } else {  // marca de canal
                p.fillRect(9, 1, 5, 10, c);
            }
            return px;
        };
        const auto add = [&](int row, int col, const QPixmap &px) {
            auto *ic = new QLabel;
            ic->setPixmap(px);
            auto *t = new QLabel;
            legendText_.push_back(t);
            lg->addWidget(ic, row, col);
            lg->addWidget(t, row, col + 1);
        };
        for (int c : {0, 3}) {
            auto *h = new QLabel;
            QFont f = h->font();
            f.setBold(true);
            h->setFont(f);
            legendText_.push_back(h);
            lg->addWidget(h, 0, c, 1, 2);
        }
        QColor wifi(255, 140, 0, 70), recA(0x2f, 0x80, 0xed, 60), recH(0x7b, 0x5c, 0xd6, 60);
        add(1, 0, swatch(0, hl));
        add(2, 0, swatch(1, QColor(0xe0, 0x70, 0x30)));
        add(3, 0, swatch(3, wifi));
        add(4, 0, swatch(2, QColor(0xd9, 0x48, 0x3b)));
        add(1, 3, swatch(4, QColor(0x3a, 0xa8, 0x5a)));
        add(2, 3, swatch(4, QColor(0xe0, 0xa0, 0x20)));
        add(3, 3, swatch(4, QColor(0xd9, 0x48, 0x3b)));
        add(4, 3, swatch(3, recA));
        add(5, 3, swatch(3, recH));
        lg->setColumnMinimumWidth(2, 24);
        lg->setColumnStretch(5, 1);
        main->addWidget(box);
    }
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    main->addWidget(summary_);
    table_ = new QTreeWidget;
    table_->setRootIsDecorated(false);
    table_->setColumnCount(6);
    table_->header()->setStretchLastSection(true);
    main->addWidget(table_, 2);
    h->addLayout(main, 1);

    connect(this, &ChannelAnalyzer::requestStart, backend, &Backend::startSurvey);
    connect(this, &ChannelAnalyzer::requestStop, backend, &Backend::stopSurvey);
    connect(backend, &Backend::surveyUpdated, this, &ChannelAnalyzer::onSurvey);
    connect(backend, &Backend::surveyStopped, this, &ChannelAnalyzer::onStopped);
    connect(backend, &Backend::statusChanged, this, [this](const RadioStatus &s) { setAdapterOpen(s.open); });

    retranslate();
    resize(1000, 640);
    recompute();
}

void ChannelAnalyzer::retranslate() {
    setWindowTitle(tr("Channel analyzer"));
    bandsTitle_->setText(tr("Bands to analyse"));
    static const char *titles[] = {"A  Boscam A", "B  Boscam B", "E  Boscam E", "F  Fatshark", "R  Raceband", nullptr};
    for (int i = 0; i < bandBoxes_.size(); i++)
        bandBoxes_[i]->setText(titles[i] ? QString::fromLatin1(titles[i]) : tr("L  Low band"));
    pilotsTitle_->setText(tr("Pilots flying together"));
    analogLbl_->setText(tr("Analog"));
    hdzLbl_->setText(QStringLiteral("HDZero"));
    narrow_->setText(tr("HDZero narrow mode"));
    narrow_->setToolTip(tr("HDZero in narrow mode takes about 17 MHz instead of 27: more of them fit."));
    airTitle_->setText(tr("On the air (tick yours)"));
    air_->setToolTip(tr("Pilots the analyzer has seen. Tick the ones that are yours: their channel is not "
                        "treated as taken."));
    startBtn_->setText(running_ ? tr("Stop") : tr("Start analysis"));
    hint_->setText(tr("Anything on the air is treated as taken: switch your own VTXs off, or tick them in the "
                      "list. It follows the last minute or so of the band. No video while it runs."));
    {
        // mismo orden que se crean en el constructor
        const QStringList t{tr("Spectrum"), tr("Channel marks"),
                            tr("Average level over the noise floor"), tr("Strongest recent burst"),
                            tr("Bursty interference (WiFi)"), tr("Pilot already on the air"),
                            tr("Clean"), tr("Some interference"), tr("Busy or noisy"),
                            tr("Recommended analog channel"), tr("Recommended HDZero channel")};
        for (int i = 0; i < legendText_.size() && i < t.size(); i++) legendText_[i]->setText(t[i]);
    }
    table_->setHeaderLabels({tr("System"), tr("Channel"), tr("MHz"), tr("Interference"), tr("Bursts"), tr("Notes")});
    table_->headerItem()->setToolTip(3, tr("Average interference level across the channel width, in dB above the noise floor."));
    table_->headerItem()->setToolTip(4, tr("Share of the time the channel is hit by intermittent signals (WiFi and similar): they come and go, much stronger than the average, which hides them. 0% = nothing intermittent."));
    recompute();
}

void ChannelAnalyzer::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) retranslate();
    QDialog::changeEvent(e);
}

void ChannelAnalyzer::setAdapterOpen(bool open) {
    if (open == open_) return;
    open_ = open;
    startBtn_->setEnabled(open_ && !bands().isEmpty());
    if (!open_ && running_) {
        running_ = false;
        startBtn_->setText(tr("Start analysis"));
        emit runningChanged(false);
    }
    if (!running_) state_->setText(open_ ? QString() : tr("Connect the adapter first."));
}

void ChannelAnalyzer::setCounts(int analog, int hdzero) {
    if (analog >= 0) nAnalog_->setValue(analog);
    if (hdzero >= 0) nHdz_->setValue(hdzero);
}

QString ChannelAnalyzer::bands() const {
    QString s;
    for (int i = 0; i < bandBoxes_.size(); i++)
        if (bandBoxes_[i]->isChecked()) s += QLatin1Char(kBands[i]);
    return s;
}

void ChannelAnalyzer::range(double &lo, double &hi) const {
    lo = 1e9;
    hi = 0;
    const std::string b = bands().toStdString();
    for (const auto &c : advisor::analog_channels(b)) {
        lo = std::min(lo, c.mhz);
        hi = std::max(hi, c.mhz);
    }
    if (hi == 0) {
        lo = 5645;
        hi = 5945;
    }
    lo -= kMargin;
    hi += kMargin;
}

void ChannelAnalyzer::toggle() {
    if (running_) {
        stop();
        return;
    }
    double lo, hi;
    range(lo, hi);
    last_ = SurveyData{};
    running_ = true;
    startBtn_->setText(tr("Stop"));
    state_->setText(tr("Analysing %1-%2 MHz...").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0));
    emit requestStart(lo, hi);
    emit runningChanged(true);
    recompute();
}

void ChannelAnalyzer::stop() {
    if (!running_) return;
    running_ = false;
    startBtn_->setText(tr("Start analysis"));
    state_->setText(last_.sweeps > 0 ? tr("Stopped after %n sweep(s).", nullptr, last_.sweeps) : QString());
    emit requestStop();
    emit runningChanged(false);
}

void ChannelAnalyzer::onStopped() {
    if (!running_) return;  // lo ha parado esta ventana (stop())
    running_ = false;
    startBtn_->setText(tr("Start analysis"));
    state_->setText(tr("Stopped: the receiver went back to scanning or watching."));
    emit runningChanged(false);
}

void ChannelAnalyzer::updateAir() {
    const QSignalBlocker b(air_);
    air_->clear();
    for (const auto &p : last_.pilots) {
        if (p.ageS >= 60) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        auto *it = new QListWidgetItem(QStringLiteral("%1  %2 MHz%3").arg(key).arg(p.freqMHz, 0, 'f', 0)
                                           .arg(p.isDigital() ? QStringLiteral("  HDZero") : QString()), air_);
        it->setData(Qt::UserRole, key);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(mine_.contains(key) ? Qt::Checked : Qt::Unchecked);
    }
    if (air_->count() == 0) {
        auto *it = new QListWidgetItem(tr("(none)"), air_);
        it->setFlags(Qt::NoItemFlags);
    }
}

void ChannelAnalyzer::closeEvent(QCloseEvent *e) {
    stop();
    QDialog::closeEvent(e);
}

void ChannelAnalyzer::onSurvey(const SurveyData &d) {
    if (!running_) return;
    last_ = d;
    updateAir();
    double lo, hi;
    range(lo, hi);
    state_->setText(d.sweeps == 0 ? tr("Analysing %1-%2 MHz: first sweep...").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0)
                                  : tr("Analysing %1-%2 MHz: %n sweep(s).", nullptr, d.sweeps)
                                        .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0));
    recompute();
}

QString ChannelAnalyzer::noteText(const advisor::Pick &p) const {
    QStringList n;
    if (p.r.busy >= 0 && p.r.busy < seenIdx_.size()) {
        const Pilot &s = last_.pilots[seenIdx_[p.r.busy]];
        n << tr("already in use (%1)").arg(s.name.isEmpty() ? QString::number(s.freqMHz, 'f', 0) : s.name);
    }
    for (auto k : p.notes) {
        switch (k) {
        case advisor::Note::TooClose: n << tr("closer than ideal to another pilot"); break;
        case advisor::Note::Imd: n << tr("intermodulation from two other pilots lands here"); break;
        case advisor::Note::Partial: n << tr("partly outside the analysed range"); break;
        case advisor::Note::Busy: break;  // ya dicho arriba
        }
    }
    if (p.r.burst > 0.1f) n << tr("WiFi-like bursts nearby");
    return n.join(QStringLiteral("; "));
}

void ChannelAnalyzer::recompute() {
    if (!view_) return;
    const std::string b = bands().toStdString();
    std::vector<advisor::Bin> bins;
    for (const auto &x : last_.bins) bins.push_back(advisor::Bin{x.mhz, x.meanDb, x.peakDb, x.duty});
    // pilotos vistos en el ultimo minuto que no son mios; seenIdx_ lleva el indice de busy a last_.pilots
    std::vector<advisor::Signal> seen;
    seenIdx_.clear();
    for (int i = 0; i < last_.pilots.size(); i++) {
        const Pilot &p = last_.pilots[i];
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        if (p.ageS >= 60 || mine_.contains(key)) continue;
        seen.push_back(advisor::Signal{p.freqMHz, p.bwMHz, p.isDigital(), p.name.toStdString()});
        seenIdx_.push_back(i);
    }
    const bool haveData = !bins.empty();
    QElapsedTimer t;
    t.start();
    const advisor::Advice a = haveData ? advisor::advise(bins, seen, advisor::analog_channels(b),
                                                         advisor::hdzero_channels(b, narrow_->isChecked()),
                                                         nAnalog_->value(), nHdz_->value())
                                       : advisor::Advice{};
    if (t.elapsed() > 100) KP_LOGW("analizador: recomendar ha tardado %lld ms", static_cast<long long>(t.elapsed()));
    double lo, hi;
    range(lo, hi);
    view_->setData(last_, a, lo, hi);

    table_->clear();
    if (!haveData) {
        summary_->setText(running_ ? tr("Waiting for the first sweep...")
                                   : tr("Choose the bands and how many pilots will fly, then start the analysis."));
        return;
    }
    QStringList names;
    for (const auto &p : a.picks) {
        names << QStringLiteral("<b>%1</b>%2").arg(QString::fromStdString(p.r.ch.name),
                                                     p.r.ch.hdzero ? QStringLiteral(" (HDZero)") : QString());
        auto *it = new QTreeWidgetItem(table_);
        it->setText(0, p.r.ch.hdzero ? QStringLiteral("HDZero") : tr("Analog"));
        it->setText(1, QString::fromStdString(p.r.ch.name));
        it->setText(2, QString::number(p.r.ch.mhz, 'f', 0));
        it->setText(3, QStringLiteral("%1 dB").arg(p.r.noise_db, 0, 'f', 1));
        it->setText(4, QStringLiteral("%1 %").arg(qRound(p.r.burst * 100)));
        it->setText(5, noteText(p));
        it->setForeground(3, ratingColour(p.r));
    }
    for (int c = 0; c < 5; c++) table_->resizeColumnToContents(c);
    QString s;
    if (a.picks.empty()) s = tr("Set how many pilots will fly.");
    else s = tr("Recommended: %1.").arg(names.join(QStringLiteral(", ")));
    if (!a.complete) s += QStringLiteral(" <span style='color:#d9483b'>") +
                          tr("Not enough room for that many pilots in these bands with safe spacing: add bands "
                             "or fly fewer at once.") + QStringLiteral("</span>");
    else if (a.worst_db >= 8) s += QStringLiteral(" <span style='color:#e0a020'>") +
                                   tr("Even the best channels have noticeable interference here.") +
                                   QStringLiteral("</span>");
    if (last_.sweeps < 3) s += QStringLiteral(" ") + tr("(still learning: let it run a few sweeps)");
    summary_->setText(s);
}
