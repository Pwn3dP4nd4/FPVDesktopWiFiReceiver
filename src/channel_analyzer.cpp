#include "channel_analyzer.h"
#include "channel_names.h"
#include "log.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QElapsedTimer>
#include <QSignalBlocker>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
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

// reglas WiFi por defecto: las de la UE en Europa (UE, EEE, Reino Unido, Suiza), las de EE. UU. fuera
int defaultRegion() {
    static const QLocale::Territory eu[] = {
        QLocale::Spain, QLocale::Portugal, QLocale::France, QLocale::Germany, QLocale::Italy, QLocale::Netherlands,
        QLocale::Belgium, QLocale::Luxembourg, QLocale::Austria, QLocale::Ireland, QLocale::Denmark, QLocale::Sweden,
        QLocale::Finland, QLocale::Poland, QLocale::Czechia, QLocale::Slovakia, QLocale::Slovenia, QLocale::Hungary,
        QLocale::Romania, QLocale::Bulgaria, QLocale::Greece, QLocale::Croatia, QLocale::Estonia, QLocale::Latvia,
        QLocale::Lithuania, QLocale::Malta, QLocale::Cyprus, QLocale::Norway, QLocale::Iceland, QLocale::Liechtenstein,
        QLocale::Switzerland, QLocale::UnitedKingdom};
    const auto t = QLocale::system().territory();
    for (auto e : eu)
        if (e == t) return 0;
    return 1;
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

void SpectrumView::setData(const SurveyData &d, const advisor::Advice &a, double lo, double hi,
                           const QStringList &mine) {
    // 156 kHz por bin es mas de lo que se ve: de 1 en 1 MHz (media de potencia, pico y rafagas los maximos)
    data_ = d;
    data_.bins.clear();
    double cell = -1, lin = 0;
    SpectrumPoint acc{};
    int n = 0;
    const auto flush = [&] {
        if (n == 0) return;
        acc.mhz = cell + 0.5;
        acc.meanDb = float(10 * std::log10(lin / n));
        data_.bins.push_back(acc);
    };
    for (const auto &b : d.bins) {
        const double c = std::floor(b.mhz);
        if (c != cell) {
            flush();
            cell = c;
            lin = 0;
            n = 0;
            acc = SpectrumPoint{};
            acc.peakDb = -100;
        }
        lin += std::pow(10.0, b.meanDb / 10.0);
        acc.peakDb = std::max(acc.peakDb, b.peakDb);
        // rafaga: intermitente (pico muy sobre su media); se guarda la parte del tiempo
        if (b.peakDb - b.meanDb > advisor::kBurstPeakOverMean) acc.duty = std::max(acc.duty, b.duty);
        n++;
    }
    flush();
    mine_ = mine;
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
            // fondo naranja mas intenso cuanto mas tiempo hay rafagas
            if (b.mhz < lo_ || b.mhz > hi_ || b.duty < 0.02f) continue;
            p.fillRect(QRectF(QPointF(X(b.mhz - 0.5), top), QPointF(X(b.mhz + 0.5), plotBottom)),
                       QColor(255, 140, 0, 25 + int(std::min(1.f, b.duty * 2) * 70)));
        }
        QPainterPath peak, mean;
        bool first = true;
        double lastX = 0;
        for (const auto &b : data_.bins) {
            if (b.mhz < lo_ || b.mhz > hi_) continue;
            const double x = X(b.mhz);
            if (first || X(b.mhz) - X(b.mhz - 1.5) < x - lastX) {  // hueco (sin datos): empezar otro tramo
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

    // redes WiFi (las que recuerda el analisis), en la fila bajo los pilotos: barra en los canales que se le han
    // visto (seguidos del mismo bloque: una red en 40/80 MHz, una etiqueta) y discontinua por el resto de su bloque
    // mientras no se sabe su ancho (sin trafico visto)
    {
        const QColor wc(0xc0, 0x60, 0x00);
        const double y = top + fm.height() + 6;
        const auto clip = [&](double mhz) { return X(std::clamp(mhz, lo_, hi_)); };
        for (const auto &g : advisor::group_wifi(advice_.wifi)) {
            bool unknownWidth = false;
            double blo = g.lo, bhi = g.hi;
            for (const auto &w : advice_.wifi)
                if (w.ch >= g.first && w.ch <= g.last) {
                    unknownWidth = unknownWidth || !w.traffic;
                    blo = w.block_lo;
                    bhi = w.block_hi;
                }
            if (unknownWidth && bhi >= lo_ && blo <= hi_) {
                p.setPen(QPen(wc, 1, Qt::DashLine));
                p.drawLine(QPointF(clip(blo), y), QPointF(clip(bhi), y));
            }
            if (g.hi < lo_ || g.lo > hi_) continue;
            p.fillRect(QRectF(QPointF(clip(g.lo), y - 2), QPointF(clip(g.hi), y + 2)), wc);
            p.setPen(wc);
            const QString t = g.first == g.last ? QStringLiteral("WiFi %1").arg(g.first)
                                                : QStringLiteral("WiFi %1\u2013%2").arg(g.first).arg(g.last);
            const double cx = X((g.lo + g.hi) / 2);
            p.drawText(QRectF(cx - 60, y + 3, 120, fm.height()), Qt::AlignHCenter | Qt::AlignTop, t);
        }
    }

    // lo que hay en el aire
    for (const auto &s : data_.pilots) {
        if (s.ageS >= 60 || s.freqMHz < lo_ || s.freqMHz > hi_) continue;
        const QString key = s.name.isEmpty() ? QString::number(s.freqMHz, 'f', 0) : s.name;
        const bool mine = mine_.contains(key);
        const QString shown = s.name.isEmpty() ? key : s.shownName();  // "R5/A4" mientras no se elija
        const QString t = shown + (s.isDigital() ? QStringLiteral(" HDZ") : QString()) +
                          (mine ? QStringLiteral(" \u2713") : QString());
        const QColor c = mine ? QColor(0x3a, 0xa8, 0x5a) : QColor(0xd9, 0x48, 0x3b);  // tuyo: verde
        p.setPen(QPen(c, 1.5, Qt::DashLine));
        p.drawLine(QPointF(X(s.freqMHz), top), QPointF(X(s.freqMHz), plotBottom));
        p.setPen(c);
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
    regionLbl_ = new QLabel;
    region_ = new QComboBox;
    region_->addItem(QString(), 0);  // UE
    region_->addItem(QString(), 1);  // EE. UU. y otros
    region_->setCurrentIndex(qBound(0, settings().value("analyzer/region", defaultRegion()).toInt(), 1));
    connect(region_, &QComboBox::currentIndexChanged, this, [this] {
        settings().setValue("analyzer/region", region_->currentData().toInt());
        wifiMem_.reset();  // los bloques cambian: se vuelve a aprender con el ultimo barrido
        feedWifi();
        recompute();
    });
    g->addWidget(regionLbl_, 3, 0);
    g->addWidget(region_, 3, 1);
    memoryLbl_ = new QLabel;
    memory_ = new QComboBox;
    for (int n : {5, 10, 20, 40}) memory_->addItem(QString(), n);
    memory_->setCurrentIndex(qBound(0, memory_->findData(settings().value("analyzer/memory", 20).toInt()), 3));
    connect(memory_, &QComboBox::currentIndexChanged, this, [this] {
        const int n = memory_->currentData().toInt();
        settings().setValue("analyzer/memory", n);
        emit requestMemory(n);
    });

    side->addLayout(g);
    side->addSpacing(8);
    airTitle_ = new QLabel;
    airTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(airTitle_);
    air_ = new QListWidget;
    air_->setMaximumHeight(110);
    connect(air_, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        const QString k = it->data(Qt::UserRole).toString();
        mine_.removeAll(k);
        if (it->checkState() == Qt::Checked) mine_ << k;
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
    {
        // memoria junto al contador de barridos: cambia como se leen
        auto *row = new QHBoxLayout;
        row->addWidget(memoryLbl_);
        row->addWidget(memory_, 1);
        side->addLayout(row);
    }
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
            } else if (kind == 5) {  // red WiFi: su canal (barra) y el resto de su bloque (discontinua)
                p.setPen(QPen(c, 1, Qt::DashLine));
                p.drawLine(0, 6, 22, 6);
                p.fillRect(7, 4, 8, 4, c);
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
        add(5, 0, swatch(5, QColor(0xc0, 0x60, 0x00)));
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
    connect(this, &ChannelAnalyzer::requestMemory, backend, &Backend::setSurveyMemory);
    emit requestMemory(memory_->currentData().toInt());
    connect(backend, &Backend::surveyUpdated, this, &ChannelAnalyzer::onSurvey);
    connect(backend, &Backend::surveyStopped, this, &ChannelAnalyzer::onStopped);
    connect(backend, &Backend::statusChanged, this, [this](const RadioStatus &s) { setAdapterOpen(s.open); });

    retranslate();
    state_->setText(tr("Connect the adapter first."));  // hasta el primer estado del backend
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
    regionLbl_->setText(tr("WiFi rules"));
    region_->setItemText(0, tr("Europe"));
    region_->setItemText(1, tr("USA and others"));
    region_->setToolTip(tr("Which 5 GHz WiFi channels exist where you fly, and how wide a network on each one can "
                           "be (in Europe, channels 144 and 169-177 do not exist)."));
    memoryLbl_->setText(tr("Remember"));
    for (int i = 0; i < memory_->count(); i++)
        memory_->setItemText(i, tr("last %n sweep(s)", nullptr, memory_->itemData(i).toInt()));
    memory_->setToolTip(tr("How many recent sweeps the graph and the advice average. Fewer: it follows changes "
                           "faster but is noisier. More: steadier, slower to forget."));
    narrow_->setToolTip(tr("HDZero in narrow mode takes about 17 MHz instead of 27: more of them fit."));
    airTitle_->setText(tr("On the air (tick yours)"));
    air_->setToolTip(tr("Pilots the analyzer has seen. Tick the ones that are yours: their channel is not "
                        "treated as taken."));
    startBtn_->setText(running_ ? tr("Stop") : tr("Start analysis"));
    hint_->setText(tr("Anything on the air is treated as taken: switch your own VTXs off, or tick them in the "
                      "list (only for this analysis). It averages the last sweeps (Remember). No video while it "
                      "runs."));
    state_->setToolTip(tr("Sweep k/N: where it is within the sweeps it remembers. The average slides: it does not "
                          "start over at N."));
    {
        // mismo orden que se crean en el constructor
        const QStringList t{tr("Spectrum"), tr("Channel marks"),
                            tr("Average level over the noise floor"), tr("Strongest recent burst"),
                            tr("Bursty interference (WiFi)"), tr("Pilot already on the air"),
                            tr("Clean"), tr("Some interference"), tr("Busy or noisy"),
                            tr("Recommended analog channel"), tr("Recommended HDZero channel"),
                            tr("WiFi seen (dashed: rest of its block while its width is unknown)")};
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
    if (!running_) state_->setText(open_ ? tr("Ready: choose the bands and press Start analysis.")
                                         : tr("Connect the adapter first."));
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
    mine_.clear();  // "mio" vale para un analisis: otro dia, otro piloto puede estar en ese canal
    wifiMem_.reset();
    clock_.start();
    surveyT_ = 0;
    updateAir();
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
    state_->setText(last_.sweeps > 0 ? tr("Stopped after %n sweep(s).", nullptr, last_.sweeps)
                                       : tr("Ready: choose the bands and press Start analysis."));
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
    // Se actualiza en el sitio: reconstruirla cada segundo perdia clics en la casilla y el desplazamiento
    QStringList keys, texts;
    for (const auto &p : last_.pilots) {
        if (p.ageS >= 60) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        if (keys.contains(key)) continue;
        keys << key;
        texts << QStringLiteral("%1  %2 MHz%3").arg(p.name.isEmpty() ? key : p.shownName()).arg(p.freqMHz, 0, 'f', 0)
                     .arg(p.isDigital() ? QStringLiteral("  HDZero") : QString());
    }
    const QSignalBlocker b(air_);
    // fuera los que ya no estan (y el "(ninguno)")
    for (int i = air_->count() - 1; i >= 0; i--)
        if (!keys.contains(air_->item(i)->data(Qt::UserRole).toString())) delete air_->takeItem(i);
    for (int k = 0; k < keys.size(); k++) {
        QListWidgetItem *it = nullptr;
        for (int i = 0; i < air_->count() && !it; i++)
            if (air_->item(i)->data(Qt::UserRole).toString() == keys[k]) it = air_->item(i);
        if (!it) {
            it = new QListWidgetItem(air_);
            it->setData(Qt::UserRole, keys[k]);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(mine_.contains(keys[k]) ? Qt::Checked : Qt::Unchecked);
        }
        if (it->text() != texts[k]) it->setText(texts[k]);
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
    // el mismo nombre que en la ventana principal (y la clave de "mio" no salta entre R5 y A4 de un barrido a otro)
    for (Pilot &p : last_.pilots) chnames::apply(p);
    surveyT_ = clock_.isValid() ? clock_.elapsed() / 1000.0 : 0;
    updateAir();
    feedWifi();
    double lo, hi;
    range(lo, hi);
    state_->setText(d.sweeps == 0 ? tr("Analysing %1-%2 MHz: first sweep...").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0)
                                  // barrido dentro de la memoria (vuelve a 1 cada N) y el total, informativo
                                  : tr("Analysing %1-%2 MHz: sweep %3/%4 (%5 in total).")
                                        .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0)
                                        .arg((d.sweeps - 1) % memory_->currentData().toInt() + 1)
                                        .arg(memory_->currentData().toInt()).arg(d.sweeps));
    recompute();
}

QString ChannelAnalyzer::noteText(const advisor::Pick &p, const std::vector<advisor::Wifi> &wifi) const {
    QStringList n;
    if (p.r.busy >= 0 && p.r.busy < seenIdx_.size()) {
        const Pilot &s = last_.pilots[seenIdx_[p.r.busy]];
        n << tr("already in use (%1)").arg(s.name.isEmpty() ? QString::number(s.freqMHz, 'f', 0) : s.shownName());
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
    if (p.r.wifi >= 0 && p.r.wifi < static_cast<int>(wifi.size())) {
        const advisor::Wifi &w = wifi[p.r.wifi];
        // encima o solo rozando su borde: la parte del canal WiFi que pisa
        const double half = advisor::width(p.r.ch) / 2;
        const double on = std::min(p.r.ch.mhz + half, w.mhz + advisor::kWifiOccupied / 2) -
                          std::max(p.r.ch.mhz - half, w.mhz - advisor::kWifiOccupied / 2);
        if (p.r.wifi_block)
            n << tr("in the block of WiFi %1 (up to %2 MHz): hit if that network uses its whole width")
                     .arg(w.ch).arg(qRound(w.block_hi - w.block_lo));
        else if (on < advisor::kWifiOccupied / 2) n << tr("its edge overlaps WiFi network %1").arg(w.ch);
        else if (!w.now) n << tr("on WiFi network %1 (seen in the last minutes, not right now)").arg(w.ch);
        else if (w.traffic) n << tr("on WiFi network %1 (in use)").arg(w.ch);
        else n << tr("on WiFi network %1 (quiet now, not when it is used)").arg(w.ch);
    }
    return n.join(QStringLiteral("; "));
}

void ChannelAnalyzer::prepare(std::vector<advisor::Bin> &bins, std::vector<advisor::Signal> &seen,
                              std::vector<advisor::Signal> *all) {
    bins.clear();
    seen.clear();
    for (const auto &x : last_.bins) bins.push_back(advisor::Bin{x.mhz, x.meanDb, x.peakDb, x.duty});
    // pilotos vistos en el ultimo minuto que no son mios; seenIdx_ lleva el indice de busy a last_.pilots
    seenIdx_.clear();
    for (int i = 0; i < last_.pilots.size(); i++) {
        const Pilot &p = last_.pilots[i];
        if (p.ageS >= 60) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        const advisor::Signal sig{p.freqMHz, p.bwMHz, p.isDigital(), p.name.toStdString()};
        if (all) all->push_back(sig);
        if (mine_.contains(key)) {
            // mis pilotos: su propia senal (y sus bandas laterales) no es interferencia (advisor::mask_signal)
            advisor::mask_signal(bins, sig);
            continue;
        }
        seen.push_back(sig);
        seenIdx_.push_back(i);
    }
}

advisor::Region ChannelAnalyzer::region() const {
    return region_ && region_->currentData().toInt() == 1 ? advisor::Region::Us : advisor::Region::Eu;
}

void ChannelAnalyzer::feedWifi() {
    if (last_.bins.isEmpty()) return;
    std::vector<advisor::Bin> bins;
    std::vector<advisor::Signal> seen, all;
    prepare(bins, seen, &all);
    // lo que tapa cualquier piloto (tambien uno mio) no es un WiFi
    wifiMem_.update(advisor::find_wifi(bins, region(), all), all, surveyT_);
}

void ChannelAnalyzer::recompute() {
    if (!view_) return;
    const std::string b = bands().toStdString();
    std::vector<advisor::Bin> bins;
    std::vector<advisor::Signal> seen;
    prepare(bins, seen);
    // las redes WiFi que recuerda este analisis (un AP callado se ve a ratos), no solo las del ultimo barrido
    const std::vector<advisor::Wifi> wifi = wifiMem_.known(surveyT_);
    const bool haveData = !bins.empty();
    QElapsedTimer t;
    t.start();
    const advisor::Advice a = haveData ? advisor::advise(bins, seen, advisor::analog_channels(b),
                                                         advisor::hdzero_channels(b, narrow_->isChecked()),
                                                         nAnalog_->value(), nHdz_->value(), &wifi)
                                       : advisor::Advice{};
    if (t.elapsed() > 100) KP_LOGW("analizador: recomendar ha tardado %lld ms", static_cast<long long>(t.elapsed()));
    double lo, hi;
    range(lo, hi);
    view_->setData(last_, a, lo, hi, mine_);

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
        it->setText(5, noteText(p, a.wifi));
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
    if (!a.wifi.empty()) {
        QStringList chs;
        for (const auto &g : advisor::group_wifi(a.wifi))
            chs << (g.first == g.last ? QString::number(g.first)
                                      : QStringLiteral("%1\u2013%2").arg(g.first).arg(g.last));
        s += QStringLiteral(" ") + tr("WiFi seen on channel(s) %1: it counts against the channels it covers, "
                                      "even while quiet.").arg(chs.join(QStringLiteral(", ")));
    }
    if (last_.sweeps < 3) s += QStringLiteral(" ") + tr("(still learning: let it run a few sweeps)");
    summary_->setText(s);
}
