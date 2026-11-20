#include "channel_analyzer.h"
#include "channel_names.h"
#include "log.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
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
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>

#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
const char kBands[] = "ABEFRL";
constexpr double kMargin = 15;  // MHz analizados a cada lado de los canales elegidos (HDZero: 13.5)

// el piloto esta en uno de estos canales fijados (mismo sistema, a menos de kMatchMhz): es de los mios
bool onPinned(const std::vector<advisor::Candidate> &pins, const Pilot &p) {
    for (const auto &c : pins)
        if (c.hdzero == p.isDigital() && std::abs(p.freqMHz - c.mhz) < race::kMatchMhz) return true;
    return false;
}

// colores de los recomendados (franja) por sistema
const QColor kAnalogColour(0x2f, 0x80, 0xed), kHdzColour(0x7b, 0x5c, 0xd6);
// seguimiento de carrera: por debajo de esta SNR (dB) el video de un piloto se tiene por flojo
constexpr float kLowSnrDb = 12;

// chincheta de un canal fijado: cabeza redonda y punta en at
void drawPin(QPainter &p, QPointF at, const QColor &c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(c, 1.5));
    p.drawLine(at, at + QPointF(0, -5));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(at + QPointF(0, -7.5), 3, 3);
    p.restore();
}

QIcon pinIcon(const QColor &c) {
    QPixmap px(12, 12);
    px.fill(Qt::transparent);
    QPainter p(&px);
    drawPin(p, QPointF(6, 11.5), c);
    return QIcon(px);
}

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

    left_ = left;
    right_ = right;
    plotBottom_ = plotBottom;
    rowH_ = rowH;
    rowsTop_ = plotBottom + fm.height() + 6;

    // recomendados: franja con su ancho; los fijados, mas intensa y con los bordes marcados (de picks: un fijado
    // fuera de las bandas elegidas no esta entre los candidatos)
    for (const auto &pk : advice_.picks) {
        const double half = advisor::width(pk.r.ch) / 2;
        const QRectF band(QPointF(X(std::max(lo_, pk.r.ch.mhz - half)), top),
                          QPointF(X(std::min(hi_, pk.r.ch.mhz + half)), plotBottom));
        if (band.width() <= 0) continue;
        QColor c = pk.r.ch.hdzero ? kHdzColour : kAnalogColour;
        c.setAlpha(pk.pinned ? 80 : 45);
        p.fillRect(band, c);
        if (pk.pinned) {
            c.setAlpha(230);
            p.setPen(QPen(c, 1.5));
            if (pk.r.ch.mhz - half >= lo_) p.drawLine(band.topLeft(), band.bottomLeft());
            if (pk.r.ch.mhz + half <= hi_) p.drawLine(band.topRight(), band.bottomRight());
        }
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

    // filas de canales: analogicos y HDZero, coloreados por interferencia; recomendados en negrita, fijados con
    // chincheta y recuadro grueso
    const auto row = [&](const std::vector<advisor::Rating> &rs, int y, const QString &title) {
        if (rs.empty()) return;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, y, left - 4, rowH), Qt::AlignRight | Qt::AlignVCenter, title);
        QFont bf = font();
        bf.setBold(true);
        const QFontMetrics bfm(bf);
        for (const auto &r : rs) {
            if (r.ch.mhz < lo_ || r.ch.mhz > hi_) continue;
            bool picked = false;
            for (const auto &pk : advice_.picks) picked = picked || (pk.r.ch.name == r.ch.name && pk.r.ch.hdzero == r.ch.hdzero);
            const bool pinned = isPinned(r.ch);
            const QColor c = ratingColour(r);
            const QRectF box(X(r.ch.mhz) - 2.5, y + 2, 5, rowH - 4);
            p.fillRect(box, c);
            if (picked || pinned) {
                const QString name = QString::fromStdString(r.ch.name);
                p.setFont(bf);
                p.setPen(pal.color(QPalette::Text));
                p.drawText(QRectF(X(r.ch.mhz) - 20, y - fm.height() + 2, 40, fm.height()), Qt::AlignCenter, name);
                p.setFont(font());
                p.setPen(QPen(pal.color(QPalette::Text), pinned ? 2 : 1));
                p.drawRect(box.adjusted(-1.5, -1.5, 1.5, 1.5));
                if (pinned)
                    drawPin(p, QPointF(X(r.ch.mhz) + bfm.horizontalAdvance(name) / 2.0 + 5, y + 1),
                            r.ch.hdzero ? kHdzColour : kAnalogColour);
            }
        }
    };
    const int rowsTop = int(rowsTop_);
    row(advice_.analog, rowsTop, tr("Analog"));
    row(advice_.hdzero, rowsTop + rowH, QStringLiteral("HDZero"));
}

bool SpectrumView::isPinned(const advisor::Candidate &c) const {
    for (const auto &pk : advice_.picks)
        if (pk.pinned && pk.r.ch.name == c.name && pk.r.ch.hdzero == c.hdzero) return true;
    return false;
}

void SpectrumView::contextMenuEvent(QContextMenuEvent *e) {
    // el canal (o los dos, analogico y HDZero, fuera de las filas) mas cerca del clic
    const double x = e->pos().x(), y = e->pos().y();
    if (x < left_ - 10 || x > right_ + 10 || right_ <= left_ || (advice_.analog.empty() && advice_.hdzero.empty())) {
        QWidget::contextMenuEvent(e);
        return;
    }
    const double mhzPerPx = (hi_ - lo_) / (right_ - left_);
    const double mhz = lo_ + (x - left_) * mhzPerPx;
    const bool inAnalog = y >= rowsTop_ && y < rowsTop_ + rowH_;
    const bool inHdz = y >= rowsTop_ + rowH_ && y < rowsTop_ + 2 * rowH_;
    const double tol = inAnalog || inHdz ? 8 * mhzPerPx : std::max(8 * mhzPerPx, 10.0);
    const auto nearest = [&](const std::vector<advisor::Rating> &rs) -> const advisor::Candidate * {
        const advisor::Candidate *best = nullptr;
        for (const auto &r : rs)
            if (std::abs(r.ch.mhz - mhz) <= tol && (!best || std::abs(r.ch.mhz - mhz) < std::abs(best->mhz - mhz)))
                best = &r.ch;
        return best;
    };
    QMenu menu(this);
    const auto offer = [&](const advisor::Candidate *c) {
        if (!c) return;
        const QString name = QString::fromStdString(c->name);
        const QString sys = c->hdzero ? QStringLiteral("HDZero") : tr("analog");
        const QString mhzText = QString::number(c->mhz, 'f', 0);
        const bool hdz = c->hdzero;
        QAction *a = menu.addAction(isPinned(*c) ? tr("Unpin %1 (%2, %3 MHz)").arg(name, sys, mhzText)
                                                 : tr("Pin %1 (%2, %3 MHz)").arg(name, sys, mhzText));
        connect(a, &QAction::triggered, this, [this, name, hdz] { emit pinToggled(name, hdz); });
    };
    if (!inHdz) offer(nearest(advice_.analog));
    if (!inAnalog) offer(nearest(advice_.hdzero));
    if (menu.isEmpty()) {
        QAction *a = menu.addAction(tr("Right-click a channel mark to pin it"));
        a->setEnabled(false);
    }
    bool any = false;
    for (const auto &pk : advice_.picks) any = any || pk.pinned;
    menu.addSeparator();
    QAction *all = menu.addAction(tr("Unpin all"));
    all->setEnabled(any);
    connect(all, &QAction::triggered, this, &SpectrumView::unpinAllRequested);
    menu.exec(e->globalPos());
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
        // los HDZero fijados cambian de ancho: el seguimiento tiene que usar el mismo que la recomendacion
        if (raceOn_) {
            move_.reset();
            race_.start(pinnedCandidates());
        }
        recompute();
        updateRace();
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
            } else if (kind == 6) {  // canal fijado: franja con bordes y chincheta
                QColor f = c;
                f.setAlpha(80);
                p.fillRect(3, 0, 16, 12, f);
                p.setPen(QPen(c, 1.5));
                p.drawLine(QPointF(3, 0), QPointF(3, 12));
                p.drawLine(QPointF(19, 0), QPointF(19, 12));
                drawPin(p, QPointF(11, 11.5), c);
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
        QColor wifi(255, 140, 0, 70), recA = kAnalogColour, recH = kHdzColour;
        recA.setAlpha(60);
        recH.setAlpha(60);
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
        add(6, 3, swatch(6, kAnalogColour));
        lg->setColumnMinimumWidth(2, 24);
        lg->setColumnStretch(5, 1);
        main->addWidget(box);
    }
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    main->addWidget(summary_);
    // pestanas bajo la grafica: la recomendacion (con los fijados) y el seguimiento de carrera
    tabs_ = new QTabWidget;
    {
        auto *page = new QWidget;
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);
        table_ = new QTreeWidget;
        table_->setRootIsDecorated(false);
        table_->setColumnCount(7);
        table_->header()->setStretchLastSection(true);
        connect(table_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *it, int col) {
            if (col != 0) return;
            const QString name = it->data(0, Qt::UserRole).toString();
            const bool hdz = it->data(0, Qt::UserRole + 1).toBool();
            const bool want = it->checkState(0) == Qt::Checked;
            // recompute() rehace la tabla: fuera de la senal de este elemento
            QTimer::singleShot(0, this, [this, name, hdz, want] {
                if (want != isPinned(name, hdz)) togglePin(name, hdz);
            });
        });
        v->addWidget(table_, 1);
        auto *r = new QHBoxLayout;
        pinHint_ = new QLabel;
        pinHint_->setWordWrap(true);
        r->addWidget(pinHint_, 1);
        unpinBtn_ = new QPushButton;
        connect(unpinBtn_, &QPushButton::clicked, this, [this] { setPins({}); });
        r->addWidget(unpinBtn_);
        v->addLayout(r);
        tabs_->addTab(page, QString());
    }
    {
        auto *page = new QWidget;
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);
        auto *top = new QHBoxLayout;
        raceBox_ = new QCheckBox;
        connect(raceBox_, &QCheckBox::toggled, this, [this](bool on) { setRace(on); });
        top->addWidget(raceBox_);
        raceState_ = new QLabel;
        raceState_->setWordWrap(true);
        top->addWidget(raceState_, 1);
        v->addLayout(top);
        raceTable_ = new QTreeWidget;
        raceTable_->setRootIsDecorated(false);
        raceTable_->setColumnCount(7);
        raceTable_->header()->setStretchLastSection(true);
        raceTable_->setSelectionMode(QAbstractItemView::NoSelection);
        v->addWidget(raceTable_, 1);
        auto *r = new QHBoxLayout;
        raceAdvice_ = new QLabel;
        raceAdvice_->setWordWrap(true);
        raceAdvice_->setTextFormat(Qt::RichText);
        r->addWidget(raceAdvice_, 1);
        applyBtn_ = new QPushButton;
        applyBtn_->setVisible(false);
        connect(applyBtn_, &QPushButton::clicked, this, &ChannelAnalyzer::applyMove);
        r->addWidget(applyBtn_);
        v->addLayout(r);
        tabs_->addTab(page, QString());
    }
    main->addWidget(tabs_, 2);
    h->addLayout(main, 1);

    pinned_ = parsePins(settings().value("analyzer/pinned").toStringList());
    connect(view_, &SpectrumView::pinToggled, this, &ChannelAnalyzer::togglePin);
    connect(view_, &SpectrumView::unpinAllRequested, this, [this] { setPins({}); });

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
    air_->setToolTip(tr("Pilots the analyzer has seen. Those on a pinned channel are already yours. Tick the other "
                        "ones that are yours: their channel is not treated as taken and the advice may give them "
                        "another one."));
    startBtn_->setText(running_ ? tr("Stop") : tr("Start analysis"));
    hint_->setText(tr("Anything on the air is treated as taken, unless it is on a pinned channel or you tick it "
                      "in the list (only for this analysis). It averages the last sweeps (Remember). No video "
                      "while it runs."));
    state_->setToolTip(tr("Sweep k/N: where it is within the sweeps it remembers. The average slides: it does not "
                          "start over at N."));
    {
        // mismo orden que se crean en el constructor
        const QStringList t{tr("Spectrum"), tr("Channel marks"),
                            tr("Average level over the noise floor"), tr("Strongest recent burst"),
                            tr("Bursty interference (WiFi)"), tr("Pilot already on the air"),
                            tr("Clean"), tr("Some interference"), tr("Busy or noisy"),
                            tr("Recommended analog channel"), tr("Recommended HDZero channel"),
                            tr("WiFi seen (dashed: rest of its block while its width is unknown)"),
                            tr("Pinned channel (right-click a mark to pin)")};
        for (int i = 0; i < legendText_.size() && i < t.size(); i++) legendText_[i]->setText(t[i]);
    }
    table_->setHeaderLabels({tr("Pin"), tr("System"), tr("Channel"), tr("MHz"), tr("Interference"), tr("Bursts"),
                             tr("Notes")});
    table_->headerItem()->setToolTip(0, tr("Tick to pin the channel you have given to a pilot: it stays in every "
                                           "analysis and the advice only chooses the rest."));
    table_->headerItem()->setToolTip(4, tr("Average interference level across the channel width, in dB above the noise floor."));
    table_->headerItem()->setToolTip(5, tr("Share of the time the channel is hit by intermittent signals (WiFi and similar): they come and go, much stronger than the average, which hides them. 0% = nothing intermittent."));
    tabs_->setTabText(0, tr("Recommendation"));
    tabs_->setTabText(1, tr("Race"));
    pinHint_->setText(tr("Tick a row to pin its channel; right-click a channel mark on the graph to pin any other. "
                         "Pinned channels are kept between analyses."));
    unpinBtn_->setText(tr("Unpin all"));
    raceBox_->setText(tr("Race tracking"));
    raceBox_->setToolTip(tr("During the race, keeps sweeping and follows each pinned channel: its pilot's signal, "
                            "dropouts, other signals on top of it and interference. Pilots on pinned channels count "
                            "as yours. Suggests a move if a pilot would clearly do better on another free channel."));
    applyBtn_->setText(tr("Apply"));
    applyBtn_->setToolTip(tr("Pins the new channel instead of the old one (tell the pilot to change it). The other "
                             "channels keep their history."));
    raceTable_->setHeaderLabels({tr("Channel"), tr("MHz"), tr("SNR (median / low)"), tr("Dropouts"), tr("Intruders"),
                                 tr("Interference"), tr("Quality")});
    raceTable_->headerItem()->setToolTip(2, tr("Signal-to-noise ratio of its pilot while seen: the median and the "
                                               "worst moments (10th percentile)."));
    raceTable_->headerItem()->setToolTip(3, tr("Sweeps in which its pilot was missing, between the first and the "
                                               "last time it was seen (take-off and landing do not count)."));
    raceTable_->headerItem()->setToolTip(4, tr("Share of the sweeps in which someone else's signal overlapped its "
                                               "channel."));
    raceTable_->headerItem()->setToolTip(5, tr("Average interference on its channel with its pilot's own signal "
                                               "removed. Interference right under the carrier of a pilot that is "
                                               "transmitting cannot be seen."));
    raceTable_->headerItem()->setToolTip(6, tr("Estimate of its video in dB (higher is better): the worst SNR minus "
                                               "what the channel and the dropouts take away. The worst one is "
                                               "highlighted."));
    recompute();
    updateRace();
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
        setRace(false);
        emit runningChanged(false);
    }
    if (!running_) state_->setText(open_ ? tr("Ready: choose the bands and press Start analysis.")
                                         : tr("Connect the adapter first."));
}

void ChannelAnalyzer::setCounts(int analog, int hdzero) {
    if (analog >= 0) nAnalog_->setValue(analog);
    if (hdzero >= 0) nHdz_->setValue(hdzero);
}

void ChannelAnalyzer::setPinnedForTest(const QStringList &pins) { setPins(parsePins(pins)); }

void ChannelAnalyzer::setMineForTest(const QStringList &keys) {
    mine_ = keys;
    updateAir();
    recompute();
}

void ChannelAnalyzer::setRaceForTest(bool on) {
    tabs_->setCurrentIndex(on ? 1 : 0);
    raceBox_->setChecked(on);
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
    updateRace();
}

void ChannelAnalyzer::stop() {
    if (!running_) return;
    running_ = false;
    startBtn_->setText(tr("Start analysis"));
    state_->setText(last_.sweeps > 0 ? tr("Stopped after %n sweep(s).", nullptr, last_.sweeps)
                                       : tr("Ready: choose the bands and press Start analysis."));
    setRace(false);
    emit requestStop();
    emit runningChanged(false);
}

void ChannelAnalyzer::onStopped() {
    if (!running_) return;  // lo ha parado esta ventana (stop())
    running_ = false;
    startBtn_->setText(tr("Start analysis"));
    state_->setText(tr("Stopped: the receiver went back to scanning or watching."));
    setRace(false);
    emit runningChanged(false);
}

void ChannelAnalyzer::updateAir() {
    // Se actualiza en el sitio: reconstruirla cada segundo perdia clics en la casilla y el desplazamiento
    QStringList keys, texts;
    QVector<bool> onPin;  // en un canal fijado: cuenta como mio sin marcarlo
    for (const auto &p : last_.pilots) {
        if (p.ageS >= 60) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        if (keys.contains(key)) continue;
        keys << key;
        onPin << pinnedMine(p);
        texts << QStringLiteral("%1  %2 MHz%3%4").arg(p.name.isEmpty() ? key : p.shownName()).arg(p.freqMHz, 0, 'f', 0)
                     .arg(p.isDigital() ? QStringLiteral("  HDZero") : QString(),
                          onPin.back() ? QStringLiteral("  (") + tr("pinned") + QLatin1Char(')') : QString());
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
        const Qt::ItemFlags f =
            onPin[k] ? (it->flags() & ~Qt::ItemIsUserCheckable) : (it->flags() | Qt::ItemIsUserCheckable);
        if (f != it->flags()) it->setFlags(f);
        const Qt::CheckState cs = onPin[k] || mine_.contains(keys[k]) ? Qt::Checked : Qt::Unchecked;
        if (it->checkState() != cs) it->setCheckState(cs);
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
    if (raceOn_) {
        // el espectro tal cual (el monitor quita los fijados) y lo que se ve en este barrido, con su SNR
        std::vector<advisor::Bin> bins;
        bins.reserve(last_.bins.size());
        for (const auto &x : last_.bins) bins.push_back(advisor::Bin{x.mhz, x.meanDb, x.peakDb, x.duty});
        std::vector<advisor::Signal> now;
        for (const Pilot &p : last_.pilots)
            if (p.ageS == 0)
                now.push_back(advisor::Signal{p.freqMHz, p.bwMHz, p.isDigital(), p.name.toStdString(),
                                              float(p.snrDb)});
        race_.update(bins, now, wifiMem_.known(surveyT_));
    }
    recompute();
    updateRace();
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
    // pilotos vistos en el ultimo minuto que no son mios; seenIdx_ lleva el indice de busy a last_.pilots. Los que
    // estan en un canal fijado tambien son mios (son los del reparto): si no, su propio canal saldria ocupado
    seenIdx_.clear();
    const std::vector<advisor::Candidate> pins = pinnedCandidates();
    const auto signal = [](const Pilot &p) {
        return advisor::Signal{p.freqMHz, p.bwMHz, p.isDigital(), p.name.toStdString(), float(p.snrDb)};
    };
    std::vector<advisor::Signal> recent;  // todos: el nivel con que se tapa a uno mio no sale de un vecino
    for (const Pilot &p : last_.pilots)
        if (p.ageS < 60) recent.push_back(signal(p));
    if (all) *all = recent;
    for (int i = 0; i < last_.pilots.size(); i++) {
        const Pilot &p = last_.pilots[i];
        if (p.ageS >= 60) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        const advisor::Signal sig = signal(p);
        if (mine_.contains(key) || onPinned(pins, p)) {
            // mis pilotos: su propia senal (y sus bandas laterales) no es interferencia (advisor::mask_signal)
            advisor::mask_signal(bins, sig, recent);
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
                                                         nAnalog_->value(), nHdz_->value(), &wifi,
                                                         pinnedCandidates())
                                       : advisor::Advice{};
    if (t.elapsed() > 100) KP_LOGW("analizador: recomendar ha tardado %lld ms", static_cast<long long>(t.elapsed()));
    double lo, hi;
    range(lo, hi);
    // mios para la grafica: los marcados y los que estan en un canal fijado
    QStringList mine = mine_;
    for (const Pilot &p : last_.pilots)
        if (p.ageS < 60 && pinnedMine(p)) mine << (p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name);
    view_->setData(last_, a, lo, hi, mine);

    const QSignalBlocker block(table_);  // las casillas de fijar: solo los clics del usuario
    table_->clear();
    const auto pinLabel = [](const QString &name, bool hdz) {
        return QStringLiteral("<b>%1</b>%2").arg(name, hdz ? QStringLiteral(" (HDZero)") : QString());
    };
    if (!haveData) {
        QString s = running_ ? tr("Waiting for the first sweep...")
                             : tr("Choose the bands and how many pilots will fly, then start the analysis.");
        if (!pinned_.isEmpty()) {
            QStringList pins;
            for (const Pin &p : pinned_) pins << pinLabel(p.name, p.hdzero);
            s += QStringLiteral(" ") + tr("Pinned: %1.").arg(pins.join(QStringLiteral(", ")));
        }
        summary_->setText(s);
        return;
    }
    // los marcados como mios (sin canal fijado) se tratan como si no estuvieran: su canal queda libre y se reparten
    // con los demas. Se dice en que canal recomendado estan ya y cuales tienen que cambiar (o fijarse si se quedan)
    QStringList ticked;      // nombre a mostrar
    QVector<double> tickedMhz;
    QVector<bool> tickedHdz;
    for (const Pilot &p : last_.pilots) {
        if (p.ageS >= 60 || pinnedMine(p)) continue;
        const QString key = p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name;
        if (!mine_.contains(key)) continue;
        ticked << (p.name.isEmpty() ? key : p.shownName());
        tickedMhz << p.freqMHz;
        tickedHdz << p.isDigital();
    }
    QVector<bool> placed(ticked.size(), false);
    QStringList names, pinnedNames;
    QFont bold = table_->font();
    bold.setBold(true);
    for (const auto &p : a.picks) {
        const QString name = QString::fromStdString(p.r.ch.name);
        (p.pinned ? pinnedNames : names) << pinLabel(name, p.r.ch.hdzero);
        auto *it = new QTreeWidgetItem(table_);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(0, p.pinned ? Qt::Checked : Qt::Unchecked);
        it->setData(0, Qt::UserRole, name);
        it->setData(0, Qt::UserRole + 1, p.r.ch.hdzero);
        if (p.pinned) it->setIcon(0, pinIcon(p.r.ch.hdzero ? kHdzColour : kAnalogColour));
        it->setToolTip(0, p.pinned ? tr("Pinned: untick to let the advice choose this pilot's channel again.")
                                   : tr("Tick to pin this channel (it is already given to a pilot)."));
        it->setText(1, p.r.ch.hdzero ? QStringLiteral("HDZero") : tr("Analog"));
        it->setText(2, name);
        if (p.pinned) it->setFont(2, bold);
        it->setText(3, QString::number(p.r.ch.mhz, 'f', 0));
        it->setText(4, QStringLiteral("%1 dB").arg(p.r.noise_db, 0, 'f', 1));
        it->setText(5, QStringLiteral("%1 %").arg(qRound(p.r.burst * 100)));
        QString note = noteText(p, a.wifi);
        if (p.pinned)
            // se da por mio sin preguntar (los fijados se guardan: otro dia puede ser otro piloto): que se vea
            for (const Pilot &pl : last_.pilots)
                if (pl.ageS < 60 && onPinned({p.r.ch}, pl)) {
                    if (!note.isEmpty()) note += QStringLiteral("; ");
                    note += tr("%1 is on the air here: taken as yours (if it is not, unpin this channel)")
                                .arg(pl.name.isEmpty() ? QString::number(pl.freqMHz, 'f', 0) : pl.shownName());
                    break;
                }
        if (!p.pinned)
            for (int k = 0; k < ticked.size(); k++)
                if (!placed[k] && tickedHdz[k] == p.r.ch.hdzero &&
                    std::abs(tickedMhz[k] - p.r.ch.mhz) < race::kMatchMhz) {
                    placed[k] = true;
                    if (!note.isEmpty()) note += QStringLiteral("; ");
                    note += tr("your pilot %1 is already here").arg(ticked[k]);
                }
        it->setText(6, note);
        it->setForeground(4, ratingColour(p.r));
    }
    for (int c = 0; c < 6; c++) table_->resizeColumnToContents(c);
    // el aviso de interferencia, por los que elige la recomendacion (lo de los fijados ya se ve en la tabla)
    float worstSuggested = 0;
    for (const auto &p : a.picks)
        if (!p.pinned) worstSuggested = std::max(worstSuggested, p.r.noise_db);
    QString s;
    if (a.picks.empty()) s = tr("Set how many pilots will fly.");
    else if (pinnedNames.isEmpty()) s = tr("Recommended: %1.").arg(names.join(QStringLiteral(", ")));
    else if (names.isEmpty()) s = tr("Pinned: %1.").arg(pinnedNames.join(QStringLiteral(", ")));
    else s = tr("Pinned: %1. Suggested for the rest: %2.").arg(pinnedNames.join(QStringLiteral(", ")),
                                                                  names.join(QStringLiteral(", ")));
    if (!a.complete) s += QStringLiteral(" <span style='color:#d9483b'>") +
                          (pinnedNames.isEmpty()
                               ? tr("Not enough room for that many pilots in these bands with safe spacing: add bands "
                                    "or fly fewer at once.")
                               : tr("Not enough room for that many pilots with safe spacing next to the pinned "
                                    "channels: unpin some, add bands or fly fewer at once.")) +
                          QStringLiteral("</span>");
    else if (worstSuggested >= 8) s += QStringLiteral(" <span style='color:#e0a020'>") +
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
    QStringList toMove;
    for (int k = 0; k < ticked.size(); k++)
        if (!placed[k]) toMove << QStringLiteral("<b>%1</b>").arg(ticked[k]);
    if (!toMove.isEmpty() && !a.picks.empty())
        s += QStringLiteral(" <span style='color:#e0a020'>") +
             (toMove.size() == 1
                  ? tr("%1 must change to one of the suggested channels (or pin its channel if it stays).")
                  : tr("%1 must change to the suggested channels (or pin their channels if they stay)."))
                 .arg(toMove.join(QStringLiteral(", "))) +
             QStringLiteral("</span>");
    if (last_.sweeps < 3) s += QStringLiteral(" ") + tr("(still learning: let it run a few sweeps)");
    summary_->setText(s);
}

// ---- canales fijados ----------------------------------------------------------------------------------------

QVector<ChannelAnalyzer::Pin> ChannelAnalyzer::parsePins(const QStringList &list) const {
    // se comprueban contra todas las bandas: un fijado sigue valiendo aunque su banda no se analice
    const auto analog = advisor::analog_channels(kBands), hdz = advisor::hdzero_channels(kBands);
    QVector<Pin> out;
    for (QString t : list) {
        t = t.trimmed();
        Pin p;
        p.hdzero = t.startsWith(QLatin1String("hdz:"), Qt::CaseInsensitive);
        p.name = (p.hdzero ? t.mid(4) : t).trimmed().toUpper();
        bool ok = false;
        for (const auto &c : p.hdzero ? hdz : analog) ok = ok || QString::fromStdString(c.name) == p.name;
        bool dup = false;
        for (const Pin &q : out) dup = dup || (q.name == p.name && q.hdzero == p.hdzero);
        if (ok && !dup) out.push_back(p);
    }
    return out;
}

std::vector<advisor::Candidate> ChannelAnalyzer::pinnedCandidates() const {
    // los mismos Candidate que advise y el seguimiento (mismo ancho: HDZero estrecho); fuera de las bandas
    // elegidas, el de la lista de todas
    const std::string b = bands().toStdString();
    const bool narrow = narrow_ && narrow_->isChecked();
    const auto analog = advisor::analog_channels(b), hdz = advisor::hdzero_channels(b, narrow);
    const auto analogAll = advisor::analog_channels(kBands), hdzAll = advisor::hdzero_channels(kBands, narrow);
    std::vector<advisor::Candidate> out;
    for (const Pin &p : pinned_) {
        const std::string n = p.name.toStdString();
        const advisor::Candidate *found = nullptr;
        for (const auto *list : {p.hdzero ? &hdz : &analog, p.hdzero ? &hdzAll : &analogAll})
            for (const auto &c : *list)
                if (!found && c.name == n) found = &c;
        if (found) out.push_back(*found);  // siempre (parsePins): pinned_ y race_.health() van a la par
    }
    return out;
}

bool ChannelAnalyzer::isPinned(const QString &name, bool hdzero) const {
    for (const Pin &p : pinned_)
        if (p.name == name && p.hdzero == hdzero) return true;
    return false;
}

void ChannelAnalyzer::savePins() const {
    QStringList l;
    for (const Pin &p : pinned_) l << (p.hdzero ? QStringLiteral("hdz:") : QString()) + p.name;
    settings().setValue("analyzer/pinned", l);
}

void ChannelAnalyzer::setPins(const QVector<Pin> &pins) {
    pinned_ = pins;
    savePins();
    move_.reset();
    // cambio a mano: el seguimiento vuelve a empezar (sin fijados, se apaga); parado, lo de la ultima carrera ya
    // no casa con los fijados
    if (raceOn_ && !pinned_.isEmpty()) race_.start(pinnedCandidates());
    else if (raceOn_) setRace(false);
    else race_.start({});
    updateAir();
    recompute();
    updateRace();
}

void ChannelAnalyzer::togglePin(const QString &name, bool hdzero) {
    QVector<Pin> pins = pinned_;
    bool removed = false;
    for (int i = pins.size() - 1; i >= 0; i--)
        if (pins[i].name == name && pins[i].hdzero == hdzero) {
            pins.removeAt(i);
            removed = true;
        }
    if (!removed) pins.push_back(Pin{name, hdzero});
    setPins(pins);
}

bool ChannelAnalyzer::pinnedMine(const Pilot &p) const { return onPinned(pinnedCandidates(), p); }

// ---- seguimiento de carrera ---------------------------------------------------------------------------------

void ChannelAnalyzer::setRace(bool on) {
    on = on && running_ && !pinned_.isEmpty();
    if (raceBox_->isChecked() != on) {
        const QSignalBlocker b(raceBox_);
        raceBox_->setChecked(on);
    }
    if (on != raceOn_) {
        raceOn_ = on;
        move_.reset();
        if (on) race_.start(pinnedCandidates());  // apagado: lo apuntado se queda a la vista
        updateAir();
        recompute();
    }
    updateRace();
}

void ChannelAnalyzer::updateRaceControls() {
    raceBox_->setEnabled(running_ && !pinned_.isEmpty());
    unpinBtn_->setEnabled(!pinned_.isEmpty());
}

void ChannelAnalyzer::updateRace() {
    if (!raceTable_) return;
    updateRaceControls();
    const auto &hs = race_.health();
    const int worst = race_.worst();
    raceTable_->clear();
    QFont bold = raceTable_->font();
    bold.setBold(true);
    const auto chName = [](const advisor::Candidate &c) {
        return QString::fromStdString(c.name) + (c.hdzero ? QStringLiteral(" HDZero") : QString());
    };
    for (int i = 0; i < static_cast<int>(hs.size()); i++) {
        const race::ChannelHealth &h = hs[i];
        auto *it = new QTreeWidgetItem(raceTable_);
        it->setIcon(0, pinIcon(h.ch.hdzero ? kHdzColour : kAnalogColour));
        it->setText(0, chName(h.ch));
        it->setText(1, QString::number(h.ch.mhz, 'f', 0));
        if (h.seen > 0) {
            it->setText(2, QStringLiteral("%1 / %2 dB").arg(h.snr_med, 0, 'f', 0).arg(h.snr_low, 0, 'f', 0));
            it->setText(6, QStringLiteral("%1 dB").arg(h.quality, 0, 'f', 0));
        } else {
            it->setText(2, h.sweeps > 0 ? tr("not seen") : QStringLiteral("-"));
            it->setText(6, QStringLiteral("-"));
        }
        it->setText(3, QString::number(h.lost));
        it->setText(4, QStringLiteral("%1 %").arg(h.sweeps > 0 ? qRound(100.0 * h.intruders / h.sweeps) : 0));
        it->setText(5, QStringLiteral("%1 dB").arg(h.channel_cost, 0, 'f', 1));
        if (i == worst && hs.size() > 1) {
            for (int c = 0; c < raceTable_->columnCount(); c++) {
                it->setBackground(c, QColor(0xd9, 0x48, 0x3b, 50));
                it->setFont(c, bold);
            }
            it->setToolTip(0, tr("The worst video so far"));
        }
    }
    for (int c = 0; c < 6; c++) raceTable_->resizeColumnToContents(c);

    // estado
    QString st;
    const int n = static_cast<int>(hs.size()), sw = race_.sweeps();
    if (pinned_.isEmpty()) st = tr("Pin the channels you have given to the pilots first (Recommendation tab).");
    else if (!raceOn_ && sw > 0) st = tr("Tracking stopped after %n sweep(s).", nullptr, sw);
    else if (!raceOn_) st = running_ ? tr("Tick it when the race starts.")
                                     : tr("Start the analysis, then tick it when the race starts.");
    else if (sw < race::kMinSweeps) st = tr("Learning the channels: sweep %1 of %2...").arg(sw).arg(race::kMinSweeps);
    else st = tr("Following %n pinned channel(s): %1 sweeps.", nullptr, n).arg(sw);
    raceState_->setText(st);

    // recomendacion: mover a un piloto, o por que no se arregla cambiando de canal
    QString adv;
    move_.reset();
    if (raceOn_) {
        const std::string b = bands().toStdString();
        move_ = race_.suggest(advisor::analog_channels(b), advisor::hdzero_channels(b, narrow_->isChecked()));
        if (move_ && (move_->from < 0 || move_->from >= n || move_->from >= pinned_.size())) move_.reset();
        if (move_) {
            const advisor::Candidate &from = hs[move_->from].ch;
            const QString cause = move_->cause == race::Cause::Intruder
                                      ? tr("someone else is transmitting on top of it")
                                      : tr("interference or WiFi on its channel");
            const QString f = QStringLiteral("<b>%1</b>").arg(QString::fromStdString(from.name));
            const QString to = QStringLiteral("<b>%1</b> (%2 MHz)")
                                   .arg(QString::fromStdString(move_->to.name))
                                   .arg(move_->to.mhz, 0, 'f', 0);
            adv = (from.hdzero ? tr("Move the HDZero pilot on %1 to %2: about +%3 dB (%4).")
                               : tr("Move the pilot on %1 to %2: about +%3 dB (%4)."))
                      .arg(f, to, QString::number(move_->gain_db, 'f', 0), cause);
        } else if (sw >= race::kMinSweeps && worst >= 0) {
            const race::ChannelHealth &h = hs[worst];
            const bool weak = h.snr_low < kLowSnrDb || h.lost > 0;
            const QString who = QStringLiteral("<b>%1</b>").arg(chName(h.ch));
            if (weak && h.penalty < race::kMoveMinGainDb)
                adv = tr("%1 has the weakest video on a clean channel: changing channel would not fix it. Check its "
                         "antennas, VTX power and distance.").arg(who);
            else if (h.penalty >= race::kMoveMinGainDb)
                // su canal es el problema, pero no hay otro libre compatible con los demas fijados
                adv = (h.sweeps > 0 && 5.0 * h.intruders > h.sweeps
                           ? tr("%1 has someone else transmitting on top of it, but no free channel compatible with "
                                "the other pinned ones would be clearly better: unpin one or add bands.")
                           : tr("%1 has interference on its channel, but no free channel compatible with the other "
                                "pinned ones would be clearly better: unpin one or add bands."))
                          .arg(who);
            else
                adv = tr("No change needed: no pilot would clearly do better on another free channel.");
        }
    }
    raceAdvice_->setText(adv);
    raceAdvice_->setStyleSheet(move_ ? QStringLiteral("color:#d9483b;") : QString());
    applyBtn_->setVisible(move_.has_value());
}

void ChannelAnalyzer::applyMove() {
    if (!move_ || move_->from < 0 || move_->from >= pinned_.size()) return;
    const race::Move m = *move_;
    move_.reset();
    // solo cambia ese fijado: lo de los demas sigue (moved, no start)
    pinned_[m.from] = Pin{QString::fromStdString(m.to.name), m.to.hdzero};
    savePins();
    race_.moved(m.from, m.to);
    updateAir();
    recompute();
    updateRace();
}
