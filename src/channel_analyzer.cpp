#include "channel_analyzer.h"
#include "analyzer_style.h"
#include "app_settings.h"
#include "channel_names.h"
#include "spectrum.h"  // kp::fpv_channels()
#include "log.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QElapsedTimer>
#include <QSignalBlocker>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>

#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
const char kBands[] = "ABEFRL";
// MHz analizados a cada lado de los canales elegidos. 15 bastaba para su propio ancho (HDZero: 13.5), pero una senal
// digital que empieza justo debajo del canal mas bajo (un DJI pegado a R1, medido) salia siempre cortada por el
// borde del primer paso y no se reconocia nunca (src/kp_ext/digital_scan.cpp une un corte solo con el paso vecino):
// con 35 hay un paso mas a cada lado que la ve entera
constexpr double kMargin = 35;
// En el aire: un piloto visto en el ultimo minuto (lo que se lista, se recomienda y se marca como mio). Pasado esto,
// el backend aun lo recuerda pero ya no cuenta
constexpr int kOnAirS = 60;
// Seguimiento de carrera: un piloto visto hace menos de esto sigue en el aire. El barrido del espectro va cada ~3 s
// pero los pilotos solo se vuelven a medir cada ~14 s (medido): con "solo los de este barrido" su propia portadora
// contaba como interferencia de su canal (30-40 dB) y como corte en 3 de cada 4 barridos
constexpr int kRaceHoldS = 30;
// Fin automatico de la carrera: ningun piloto en su canal fijado durante esto (s, contados desde que dejan de estar
// en lo que se pasa al seguimiento, que ya los guarda kRaceHoldS: en total ~1,5 min sin verlos)
constexpr int kLandedS = 60;

// el piloto esta en uno de estos canales fijados (mismo sistema, a menos de kMatchMhz): es de los mios
bool onPinned(const std::vector<advisor::Candidate> &pins, const Pilot &p) {
    for (const auto &c : pins)
        if (c.hdzero == p.isDigital() && std::abs(p.freqMHz - c.mhz) < race::kMatchMhz) return true;
    return false;
}

// clave de un piloto en el aire: su canal ("R4") o, si no coincide con ninguno, sus MHz (la de "mio")
QString pilotKey(const Pilot &p) { return p.name.isEmpty() ? QString::number(p.freqMHz, 'f', 0) : p.name; }
// como se muestra ("R5/A4" mientras no se elija cual es)
QString pilotShown(const Pilot &p) { return p.name.isEmpty() ? pilotKey(p) : chnames::resolved(p).label(); }
bool onAir(const Pilot &p) { return p.ageS < kOnAirS; }
advisor::Signal toSignal(const Pilot &p) {
    return advisor::Signal{p.freqMHz, p.bwMHz, p.isDigital(), p.name.toStdString(), float(p.snrDb), p.ageS == 0};
}
// que senal digital es, para la grafica ("HDZ") y la lista ("HDZero"); vacio = analogica
QString digitalTag(const Pilot &p, bool shortTag) {
    if (!p.isDigital()) return QString();
    if (p.digital.startsWith(QLatin1String("hdzero"))) return shortTag ? QStringLiteral("HDZ") : QStringLiteral("HDZero");
    if (p.digital == QLatin1String("dji")) return QStringLiteral("DJI");
    return shortTag ? QStringLiteral("DIG") : QStringLiteral("digital");
}
QString chName(const advisor::Candidate &c) {
    return QString::fromStdString(c.name) + (c.hdzero ? QStringLiteral(" HDZero") : QString());
}

// barra de barrido
const QColor kSweepColour(0x1e, 0x90, 0xff);

// Numero de pilotos para el auto-inicio: el maximo (todos los fijados) se muestra con un texto ("todos")
class AllSpinBox : public QSpinBox {
public:
    QString allText;
    void setAllText(const QString &t) {  // y vuelve a escribir el valor mostrado (cambio de idioma)
        allText = t;
        lineEdit()->setText(textFromValue(value()));
        updateGeometry();  // el ancho depende del texto (idioma)
    }
    // QSpinBox calcula su ancho con los numeros: se le suma lo que "todos" sea mas ancho que el mas largo
    QSize sizeHint() const override { return widen(QSpinBox::sizeHint()); }
    QSize minimumSizeHint() const override { return widen(QSpinBox::minimumSizeHint()); }

protected:
    QString textFromValue(int v) const override {
        return v == maximum() && !allText.isEmpty() ? allText : QSpinBox::textFromValue(v);
    }
    int valueFromText(const QString &t) const override {
        return t.compare(allText, Qt::CaseInsensitive) == 0 ? maximum() : QSpinBox::valueFromText(t);
    }
    QSize widen(QSize s) const {
        const QFontMetrics fm(font());
        const int extra = fm.horizontalAdvance(allText + QLatin1Char(' ')) -
                          fm.horizontalAdvance(QString::number(std::max(std::abs(minimum()), std::abs(maximum()))));
        if (extra > 0) s.rwidth() += extra;
        return s;
    }
    QValidator::State validate(QString &t, int &pos) const override {
        if (!allText.isEmpty() && allText.startsWith(t, Qt::CaseInsensitive))
            return t.size() == allText.size() ? QValidator::Acceptable : QValidator::Intermediate;
        return QSpinBox::validate(t, pos);
    }
};

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

// Las muestras de la leyenda del analizador (22x12), como se dibuja cada cosa en el espectro
enum class Swatch {
    Area,     // relleno y borde arriba (nivel medio)
    Line,     // linea (pico reciente)
    DashedV,  // discontinua vertical (piloto en el aire)
    Band,     // fondo/franja (WiFi, canal recomendado)
    Mark,     // marca de canal
    Wifi,     // red WiFi: su canal (barra) y el resto de su bloque (discontinua)
    Pinned,   // canal fijado: franja con bordes y chincheta
    Sweep,    // barra de barrido: recien medido a la derecha, apagandose hacia atras
};

QPixmap swatch(Swatch kind, const QColor &c) {
    QPixmap px(22, 12);
    px.fill(Qt::transparent);
    QPainter p(&px);
    switch (kind) {
    case Swatch::Area: {
        QColor f = c;
        f.setAlpha(90);
        p.fillRect(0, 3, 22, 9, f);
        p.setPen(QPen(c, 1.5));
        p.drawLine(0, 3, 22, 3);
        break;
    }
    case Swatch::Line:
        p.setPen(QPen(c, 1.5));
        p.drawLine(0, 6, 22, 6);
        break;
    case Swatch::DashedV:
        p.setPen(QPen(c, 1.5, Qt::DashLine));
        p.drawLine(11, 0, 11, 12);
        break;
    case Swatch::Band:
        p.fillRect(0, 0, 22, 12, c);
        break;
    case Swatch::Pinned: {
        QColor f = c;
        f.setAlpha(80);
        p.fillRect(3, 0, 16, 12, f);
        p.setPen(QPen(c, 1.5));
        p.drawLine(QPointF(3, 0), QPointF(3, 12));
        p.drawLine(QPointF(19, 0), QPointF(19, 12));
        drawPin(p, QPointF(11, 11.5), c);
        break;
    }
    case Swatch::Wifi:
        p.setPen(QPen(c, 1, Qt::DashLine));
        p.drawLine(0, 6, 22, 6);
        p.fillRect(7, 4, 8, 4, c);
        break;
    case Swatch::Sweep: {
        QLinearGradient g(0, 0, 20, 0);
        QColor a = c;
        a.setAlpha(50);
        g.setColorAt(0, a);
        g.setColorAt(1, c);
        p.fillRect(0, 4, 20, 5, g);
        p.setPen(QPen(c, 2));
        p.drawLine(QPointF(20.5, 2), QPointF(20.5, 11));
        break;
    }
    case Swatch::Mark:
        p.fillRect(9, 1, 5, 10, c);
        break;
    }
    return px;
}
}  // namespace

// ---- grafica ------------------------------------------------------------------------------------------------

SpectrumView::SpectrumView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sweepClock_.start();
    anim_ = new QTimer(this);
    anim_->setInterval(150);
    connect(anim_, &QTimer::timeout, this, [this] {
        if (isVisible()) update();
    });
}

void SpectrumView::setScanning(bool on) {
    if (on && !scanning_) {
        cellSeq_.clear();
        cellT_.clear();
        head_.clear();
        headT_ = -1;
        periodS_ = 3;
    }
    scanning_ = on;
    if (on) anim_->start();
    else anim_->stop();
    update();
}

void SpectrumView::trackSweep() {
    // seq crece con cada paso medido: una celda cuyo seq ha cambiado se ha vuelto a medir desde la actualizacion
    // anterior (setData tambien llega sin espectro nuevo, al recalcular: ahi no cambia ninguna)
    const qint64 t = sweepClock_.elapsed();
    QVector<int> fresh;
    std::vector<double> gaps;
    for (const auto &b : data_.bins) {
        if (b.seq == 0) continue;
        const int c = int(std::floor(b.mhz));
        const auto it = cellSeq_.find(c);
        if (it != cellSeq_.end() && it.value() == b.seq) continue;
        if (it != cellSeq_.end()) {
            const auto ot = cellT_.find(c);
            if (ot != cellT_.end()) gaps.push_back((t - ot.value()) / 1000.0);
        }
        cellSeq_[c] = b.seq;
        cellT_[c] = t;
        fresh << c;
    }
    if (fresh.isEmpty()) return;
    head_ = fresh;
    headT_ = t;
    if (!gaps.empty()) {  // mediana de lo que han tardado en volver a medirse
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        periodS_ = std::clamp(0.7 * periodS_ + 0.3 * gaps[gaps.size() / 2], 0.5, 30.0);
    }
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
        acc.seq = std::max(acc.seq, b.seq);
        // rafaga: intermitente (pico muy sobre su media); se guarda la parte del tiempo
        if (b.peakDb - b.meanDb > advisor::kBurstPeakOverMean) acc.duty = std::max(acc.duty, b.duty);
        n++;
    }
    flush();
    trackSweep();
    mine_ = mine;
    advice_ = a;
    lo_ = lo;
    hi_ = hi;
    update();
}

void SpectrumView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());
    const QFontMetrics fm(font());
    Geometry g{fm};
    g.rowH = fm.height() + 4;
    g.left = std::max({fm.horizontalAdvance(QStringLiteral("40 dB")), fm.horizontalAdvance(tr("Analog")),
                       fm.horizontalAdvance(QStringLiteral("HDZero"))}) + 10;
    g.right = width() - 8;
    // barra de barrido arriba del todo; la grafica, debajo
    g.barTop = 7;
    g.barH = 7;
    g.top = g.barTop + g.barH + 14;
    g.plotBottom = height() - 2 * g.rowH - fm.height() - 10;
    g.lo = lo_;
    g.hi = hi_;
    // geometria del ultimo pintado (menu contextual): eje x y filas de canales
    left_ = g.left;
    right_ = g.right;
    plotBottom_ = g.plotBottom;
    rowH_ = g.rowH;
    rowsTop_ = g.plotBottom + fm.height() + 6;

    drawAxes(p, g);
    drawPicks(p, g);
    drawSpectrum(p, g);
    drawWifi(p, g);
    drawSweepBar(p, g);
    drawAir(p, g);
    drawChannelRows(p, g);
}

// rejilla y ejes: la rejilla, el color del texto muy tenue; los textos, el del texto (QPalette::Mid es casi negro
// en tema oscuro: ni la rejilla ni los dB se veian)
void SpectrumView::drawAxes(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    QColor grid = pal.color(QPalette::Text);
    grid.setAlpha(70);
    for (int db = 10; db <= 40; db += 10) {
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(g.left, g.y(db)), QPointF(g.right, g.y(db)));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, g.y(db) - g.fm.height() / 2.0, g.left - 4, g.fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 dB").arg(db));
    }
    p.setPen(pal.color(QPalette::Text));
    for (int f = int(std::ceil(lo_ / 50) * 50); f <= hi_; f += 50) {
        p.drawLine(QPointF(g.x(f), g.plotBottom), QPointF(g.x(f), g.plotBottom + 3));
        p.drawText(QRectF(g.x(f) - 30, g.plotBottom + 3, 60, g.fm.height()), Qt::AlignCenter, QString::number(f));
    }
    p.drawLine(QPointF(g.left, g.plotBottom), QPointF(g.right, g.plotBottom));
}

// recomendados: franja con su ancho; los fijados, mas intensa y con los bordes marcados (de picks: un fijado
// fuera de las bandas elegidas no esta entre los candidatos)
void SpectrumView::drawPicks(QPainter &p, const Geometry &g) const {
    for (const auto &pk : advice_.picks) {
        const double half = advisor::width(pk.r.ch) / 2;
        const QRectF band(QPointF(g.x(std::max(lo_, pk.r.ch.mhz - half)), g.top),
                          QPointF(g.x(std::min(hi_, pk.r.ch.mhz + half)), g.plotBottom));
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
}

// espectro: rafagas (fondo naranja), picos (linea fina) y media (area)
void SpectrumView::drawSpectrum(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    if (data_.bins.isEmpty()) return;
    for (const auto &b : data_.bins) {
        // fondo naranja mas intenso cuanto mas tiempo hay rafagas
        if (b.mhz < lo_ || b.mhz > hi_ || b.duty < 0.02f) continue;
        p.fillRect(QRectF(QPointF(g.x(b.mhz - 0.5), g.top), QPointF(g.x(b.mhz + 0.5), g.plotBottom)),
                   QColor(255, 140, 0, 25 + int(std::min(1.f, b.duty * 2) * 70)));
    }
    QPainterPath peak, mean;
    bool first = true;
    double lastX = 0;
    for (const auto &b : data_.bins) {
        if (b.mhz < lo_ || b.mhz > hi_) continue;
        const double x = g.x(b.mhz);
        if (first || g.x(b.mhz) - g.x(b.mhz - 1.5) < x - lastX) {  // hueco (sin datos): empezar otro tramo
            peak.moveTo(x, g.y(b.peakDb));
            mean.moveTo(x, g.plotBottom);
        } else {
            peak.lineTo(x, g.y(b.peakDb));
        }
        mean.lineTo(x, g.y(b.meanDb));
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
void SpectrumView::drawWifi(QPainter &p, const Geometry &g) const {
    const QColor wc(0xc0, 0x60, 0x00);
    const double y = g.top + g.fm.height() + 6;
    const auto clip = [&](double mhz) { return g.x(std::clamp(mhz, lo_, hi_)); };
    for (const auto &grp : advisor::group_wifi(advice_.wifi)) {
        bool unknownWidth = false;
        double blo = grp.lo, bhi = grp.hi;
        for (const auto &w : advice_.wifi)
            if (w.ch >= grp.first && w.ch <= grp.last) {
                unknownWidth = unknownWidth || !w.traffic;
                blo = w.block_lo;
                bhi = w.block_hi;
            }
        if (unknownWidth && bhi >= lo_ && blo <= hi_) {
            p.setPen(QPen(wc, 1, Qt::DashLine));
            p.drawLine(QPointF(clip(blo), y), QPointF(clip(bhi), y));
        }
        if (grp.hi < lo_ || grp.lo > hi_) continue;
        p.fillRect(QRectF(QPointF(clip(grp.lo), y - 2), QPointF(clip(grp.hi), y + 2)), wc);
        p.setPen(wc);
        const QString t = grp.first == grp.last ? QStringLiteral("WiFi %1").arg(grp.first)
                                            : QStringLiteral("WiFi %1\u2013%2").arg(grp.first).arg(grp.last);
        const double cx = g.x((grp.lo + grp.hi) / 2);
        p.drawText(QRectF(cx - 60, y + 3, 120, g.fm.height()), Qt::AlignHCenter | Qt::AlignTop, t);
    }
}

// barra de barrido: cada MHz, del color de la seleccion recien medido y apagandose hasta que vuelve a pasar el
// barrido (periodS_); gris lo que no se mide desde hace mas de 2 pasadas. Donde va ahora, una franja tenue sobre
// la grafica y una raya en la barra por el borde que avanza
void SpectrumView::drawSweepBar(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    QColor track = pal.color(QPalette::Text);
    track.setAlpha(28);
    p.fillRect(QRectF(g.left, g.barTop, g.right - g.left, g.barH), track);
    const qint64 t = sweepClock_.elapsed();
    const QColor hl = kSweepColour;  // azul fijo: el de la seleccion depende del tema (gris o el de acento)
    if (scanning_) {
        for (auto it = cellT_.constBegin(); it != cellT_.constEnd(); ++it) {
            const double mhz = it.key();
            if (mhz + 1 < lo_ || mhz > hi_) continue;
            const double age = (t - it.value()) / 1000.0 / periodS_;
            QColor c = hl;
            if (age > 2) {
                c = pal.color(QPalette::Text);
                c.setAlpha(60);
            } else {
                c.setAlpha(int(255 * std::clamp(1.0 - 0.8 * age, 0.25, 1.0)));
            }
            p.fillRect(QRectF(QPointF(g.x(std::max(lo_, mhz)), g.barTop),
                              QPointF(g.x(std::min(hi_, mhz + 1)), g.barTop + g.barH)),
                       c);
        }
        drawSweepHead(p, g, t);
    }
    // titulo a la izquierda, si cabe
    QFont sf = font();
    sf.setPointSizeF(std::max(6.0, sf.pointSizeF() * 0.8));
    const QFontMetrics sfm(sf);
    p.setFont(sf);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(QRectF(0, g.barTop - 4, g.left - 4, g.barH + 8), Qt::AlignRight | Qt::AlignVCenter,
               sfm.elidedText(tr("Sweep"), Qt::ElideRight, g.left - 6));
    p.setFont(font());
}

void SpectrumView::drawSweepHead(QPainter &p, const Geometry &g, qint64 t) const {
    const QColor hl = kSweepColour;
    const double fade = headT_ < 0 ? 0 : std::clamp(1.0 - (t - headT_) / 1000.0 / periodS_, 0.0, 1.0);
    if (!head_.isEmpty() && fade > 0) {
        QColor band = hl, edge = hl;
        band.setAlpha(int(60 * fade));
        edge.setAlpha(int(200 * fade));
        // solo el tramo de head_ que acaba en lo ultimo medido (el seq mas alto), y su borde ahi: una
        // actualizacion puede cubrir el final de un barrido y el principio del siguiente
        QVector<int> h = head_;
        std::sort(h.begin(), h.end());
        int newest = 0;
        for (int k = 1; k < h.size(); k++)
            if (cellSeq_.value(h[k]) > cellSeq_.value(h[newest])) newest = k;
        {
            int i = newest;
            while (i > 0 && h[i] - h[i - 1] <= 2) i--;
            const double a = std::max(lo_, double(h[i])), b = std::min(hi_, double(h[newest] + 1));
            if (b > a) {
                // estela: transparente por detras y mas intensa hacia el borde que avanza (los pasos van de
                // menos a mas MHz, src/scan_plan.cpp); una franja plana se confundia con un canal recomendado
                QLinearGradient grad(g.x(a), 0, g.x(b), 0);
                QColor clear = band;
                clear.setAlpha(0);
                grad.setColorAt(0, clear);
                grad.setColorAt(1, band);
                p.fillRect(QRectF(QPointF(g.x(a), g.top), QPointF(g.x(b), g.plotBottom)), grad);
                QColor line = edge;
                line.setAlpha(int(110 * fade));
                p.setPen(QPen(line, 1, Qt::DotLine));
                p.drawLine(QPointF(g.x(b), g.top), QPointF(g.x(b), g.plotBottom));
                p.setPen(QPen(edge, 2));
                p.drawLine(QPointF(g.x(b), g.barTop - 2), QPointF(g.x(b), g.barTop + g.barH + 2));
            }
        }
    }
}

// lo que hay en el aire
void SpectrumView::drawAir(QPainter &p, const Geometry &g) const {
    for (const auto &s : data_.pilots) {
        if (!onAir(s) || s.freqMHz < lo_ || s.freqMHz > hi_) continue;
        const QString key = pilotKey(s);
        const bool mine = mine_.contains(key);
        const QString shown = pilotShown(s);  // "R5/A4" mientras no se elija
        const QString t = shown + (s.isDigital() ? QLatin1Char(' ') + digitalTag(s, true) : QString()) +
                          (mine ? QStringLiteral(" \u2713") : QString());
        const QColor c = mine ? QColor(0x3a, 0xa8, 0x5a) : QColor(0xd9, 0x48, 0x3b);  // tuyo: verde
        p.setPen(QPen(c, 1.5, Qt::DashLine));
        p.drawLine(QPointF(g.x(s.freqMHz), g.top), QPointF(g.x(s.freqMHz), g.plotBottom));
        p.setPen(c);
        p.drawText(QPointF(g.x(s.freqMHz) + 3, g.top + g.fm.ascent()), t);
    }
}

// filas de canales: analogicos y HDZero, coloreados por interferencia; recomendados en negrita, fijados con
// chincheta y recuadro grueso
void SpectrumView::drawChannelRows(QPainter &p, const Geometry &g) const {
    const QPalette pal = palette();
    const auto row = [&](const std::vector<advisor::Rating> &rs, int y, const QString &title) {
        if (rs.empty()) return;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(0, y, g.left - 4, g.rowH), Qt::AlignRight | Qt::AlignVCenter, title);
        QFont bf = font();
        bf.setBold(true);
        const QFontMetrics bfm(bf);
        for (const auto &r : rs) {
            if (r.ch.mhz < lo_ || r.ch.mhz > hi_) continue;
            bool picked = false;
            for (const auto &pk : advice_.picks) picked = picked || (pk.r.ch.name == r.ch.name && pk.r.ch.hdzero == r.ch.hdzero);
            const bool pinned = isPinned(r.ch);
            const QColor c = ratingColour(r);
            const QRectF box(g.x(r.ch.mhz) - 2.5, y + 2, 5, g.rowH - 4);
            p.fillRect(box, c);
            if (picked || pinned) {
                const QString name = QString::fromStdString(r.ch.name);
                p.setFont(bf);
                p.setPen(pal.color(QPalette::Text));
                p.drawText(QRectF(g.x(r.ch.mhz) - 20, y - g.fm.height() + 2, 40, g.fm.height()), Qt::AlignCenter, name);
                p.setFont(font());
                p.setPen(QPen(pal.color(QPalette::Text), pinned ? 2 : 1));
                p.drawRect(box.adjusted(-1.5, -1.5, 1.5, 1.5));
                if (pinned)
                    drawPin(p, QPointF(g.x(r.ch.mhz) + bfm.horizontalAdvance(name) / 2.0 + 5, y + 1),
                            r.ch.hdzero ? kHdzColour : kAnalogColour);
            }
        }
    };
    const int rowsTop = int(rowsTop_);
    row(advice_.analog, rowsTop, tr("Analog"));
    row(advice_.hdzero, rowsTop + g.rowH, QStringLiteral("HDZero"));
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

    h->addWidget(buildSide());

    auto *main = new QVBoxLayout;
    view_ = new SpectrumView;
    main->addWidget(view_, 3);
    main->addWidget(buildLegend());
    summary_ = new QLabel;
    summary_->setObjectName(QStringLiteral("summary"));
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    main->addWidget(summary_);
    // pestanas bajo la grafica: la recomendacion (con los fijados) y el seguimiento de carrera
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("tabs"));
    tabs_->addTab(buildAdvicePage(), QString());
    tabs_->addTab(buildRacePage(), QString());
    // historial: lo que ha ido avisando y recomendando la carrera, para verlo al final (src/race_history.h)
    history_ = new RaceHistory;
    connect(history_, &RaceHistory::eventsChanged, this, &ChannelAnalyzer::updateHistoryTab);
    connect(history_, &RaceHistory::clearRequested, this, [this] { history_->clear(raceOn_); });
    tabs_->addTab(history_, QString());
    main->addWidget(tabs_, 2);
    // con la pestana oculta el video de cada piloto no se refresca (updateRace): al abrirla, lo de ahora
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 2) history_->refreshPilots();
    });
    h->addLayout(main, 1);

    pinned_ = parsePins(appSettings().value("analyzer/pinned").toStringList());
    connect(view_, &SpectrumView::pinToggled, this, &ChannelAnalyzer::togglePin);
    connect(view_, &SpectrumView::unpinAllRequested, this, [this] { setPins({}); });
    connect(this, &ChannelAnalyzer::runningChanged, view_, &SpectrumView::setScanning);

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

// las bandas a analizar: una casilla por banda y, al lado, el desplegable de sus canales
void ChannelAnalyzer::addBandsPanel(QVBoxLayout *side) {
    bandsTitle_ = new QLabel;
    bandsTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(bandsTitle_);
    const QString saved = appSettings().value("analyzer/bands", QStringLiteral("ABEFR")).toString();
    excluded_ = appSettings().value("analyzer/excluded").toStringList();
    for (const char *b = kBands; *b; b++) {
        auto *row = new QHBoxLayout;
        auto *c = new QCheckBox;
        c->setChecked(saved.contains(QLatin1Char(*b)));
        connect(c, &QCheckBox::toggled, this, &ChannelAnalyzer::bandsChanged);
        // al lado, el desplegable para quitar canales sueltos de la banda
        auto *btn = new QToolButton;
        btn->setObjectName(QStringLiteral("channels:") + QLatin1Char(*b));  // "channels:R"
        btn->setArrowType(Qt::DownArrow);
        btn->setAutoRaise(true);
        const int band = bandBoxes_.size();
        connect(btn, &QToolButton::clicked, this, [this, band, btn] { showChannelMenu(band, btn); });
        bandBoxes_.push_back(c);
        bandBtns_.push_back(btn);
        row->addWidget(c, 1);
        row->addWidget(btn);
        side->addLayout(row);
    }
}

// cuantos pilotos (analogicos, HDZero), HDZero estrecho, reglas WiFi y memoria
void ChannelAnalyzer::addPilotsPanel(QVBoxLayout *side) {
    pilotsTitle_ = new QLabel;
    pilotsTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(pilotsTitle_);
    auto *g = new QGridLayout;
    analogLbl_ = new QLabel;
    hdzLbl_ = new QLabel;
    nAnalog_ = new QSpinBox;
    nAnalog_->setObjectName(QStringLiteral("analogCount"));
    nHdz_ = new QSpinBox;
    nHdz_->setObjectName(QStringLiteral("hdzeroCount"));
    nAnalog_->setRange(0, 8);
    nHdz_->setRange(0, 8);
    nAnalog_->setValue(appSettings().value("analyzer/analog", 4).toInt());
    nHdz_->setValue(appSettings().value("analyzer/hdzero", 0).toInt());
    for (QSpinBox *s : {nAnalog_, nHdz_})
        connect(s, &QSpinBox::valueChanged, this, [this] {
            appSettings().setValue("analyzer/analog", nAnalog_->value());
            appSettings().setValue("analyzer/hdzero", nHdz_->value());
            recompute();
        });
    g->addWidget(analogLbl_, 0, 0);
    g->addWidget(nAnalog_, 0, 1);
    g->addWidget(hdzLbl_, 1, 0);
    g->addWidget(nHdz_, 1, 1);
    narrow_ = new QCheckBox;
    narrow_->setChecked(appSettings().value("analyzer/narrow", false).toBool());
    connect(narrow_, &QCheckBox::toggled, this, [this](bool on) {
        appSettings().setValue("analyzer/narrow", on);
        // los HDZero fijados cambian de ancho: el seguimiento tiene que usar el mismo que la recomendacion. Solo
        // vuelven a empezar ellos; lo de los analogicos sigue
        if (raceOn_) {
            moves_.clear();
            const auto pins = pinnedCandidates();
            const auto &hs = race_.health();
            for (int i = 0; i < static_cast<int>(pins.size()) && i < static_cast<int>(hs.size()); i++)
                if (pins[i].hdzero) race_.moved(i, pins[i]);
        }
        recompute();
        updateRace();
    });
    g->addWidget(narrow_, 2, 0, 1, 2);
    regionLbl_ = new QLabel;
    region_ = new QComboBox;
    region_->addItem(QString(), 0);  // UE
    region_->addItem(QString(), 1);  // EE. UU. y otros
    region_->setCurrentIndex(qBound(0, appSettings().value("analyzer/region", defaultRegion()).toInt(), 1));
    connect(region_, &QComboBox::currentIndexChanged, this, [this] {
        appSettings().setValue("analyzer/region", region_->currentData().toInt());
        wifiMem_.reset();  // los bloques cambian: se vuelve a aprender con el ultimo barrido
        feedWifi();
        recompute();
    });
    g->addWidget(regionLbl_, 3, 0);
    g->addWidget(region_, 3, 1);
    memoryLbl_ = new QLabel;
    memory_ = new QComboBox;
    for (int n : {5, 10, 20, 40}) memory_->addItem(QString(), n);
    memory_->setCurrentIndex(qBound(0, memory_->findData(appSettings().value("analyzer/memory", 20).toInt()), 3));
    connect(memory_, &QComboBox::currentIndexChanged, this, [this] {
        const int n = memory_->currentData().toInt();
        appSettings().setValue("analyzer/memory", n);
        emit requestMemory(n);
    });

    side->addLayout(g);
}

// panel de la izquierda: bandas, pilotos, en el aire, Empezar y el estado
QWidget *ChannelAnalyzer::buildSide() {
    auto *side = new QVBoxLayout;
    addBandsPanel(side);
    side->addSpacing(8);
    addPilotsPanel(side);
    side->addSpacing(8);
    airTitle_ = new QLabel;
    airTitle_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    side->addWidget(airTitle_);
    air_ = new QListWidget;
    air_->setObjectName(QStringLiteral("onTheAir"));
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
    startBtn_->setObjectName(QStringLiteral("start"));
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
    return sideW;
}

// leyenda: muestra dibujada + texto, en dos columnas (espectro | marcas de canal)
QWidget *ChannelAnalyzer::buildLegend() {
    // leyenda: muestra dibujada + texto, en dos columnas (espectro | marcas de canal)
    auto *box = new QWidget;
    auto *lg = new QGridLayout(box);
    lg->setContentsMargins(0, 2, 0, 2);
    lg->setHorizontalSpacing(6);
    lg->setVerticalSpacing(1);
    const QColor hl = palette().color(QPalette::Highlight);
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
    add(1, 0, swatch(Swatch::Area, hl));
    add(2, 0, swatch(Swatch::Line, QColor(0xe0, 0x70, 0x30)));
    add(3, 0, swatch(Swatch::Band, wifi));
    add(4, 0, swatch(Swatch::DashedV, QColor(0xd9, 0x48, 0x3b)));
    add(1, 3, swatch(Swatch::Mark, QColor(0x3a, 0xa8, 0x5a)));
    add(2, 3, swatch(Swatch::Mark, QColor(0xe0, 0xa0, 0x20)));
    add(3, 3, swatch(Swatch::Mark, QColor(0xd9, 0x48, 0x3b)));
    add(4, 3, swatch(Swatch::Band, recA));
    add(5, 3, swatch(Swatch::Band, recH));
    add(5, 0, swatch(Swatch::Wifi, QColor(0xc0, 0x60, 0x00)));
    add(6, 3, swatch(Swatch::Pinned, kAnalogColour));
    add(6, 0, swatch(Swatch::Sweep, kSweepColour));
    lg->setColumnMinimumWidth(2, 24);
    lg->setColumnStretch(5, 1);
    return box;
}

// pestana Recomendacion: los canales recomendados y los fijados
QWidget *ChannelAnalyzer::buildAdvicePage() {
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    table_ = new QTreeWidget;
    table_->setObjectName(QStringLiteral("advice"));
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
    return page;
}

// pestana Carrera: el seguimiento, el auto-inicio y las recomendaciones de mover
QWidget *ChannelAnalyzer::buildRacePage() {
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    auto *top = new QHBoxLayout;
    raceBox_ = new QCheckBox;
    raceBox_->setObjectName(QStringLiteral("race"));
    connect(raceBox_, &QCheckBox::toggled, this, [this](bool on) { setRace(on); });
    top->addWidget(raceBox_);
    raceState_ = new QLabel;
    raceState_->setObjectName(QStringLiteral("raceState"));
    raceState_->setWordWrap(true);
    top->addWidget(raceState_, 1);
    v->addLayout(top);
    // auto-inicio: se arma y empieza solo cuando estan en el aire los pilotos elegidos (0 = todos los fijados)
    auto *arm = new QHBoxLayout;
    armBtn_ = new QPushButton;
    armBtn_->setObjectName(QStringLiteral("arm"));
    armBtn_->setCheckable(true);
    connect(armBtn_, &QPushButton::toggled, this, [this](bool on) { setArmed(on); });
    arm->addWidget(armBtn_);
    armLbl_ = new QLabel;
    arm->addWidget(armLbl_);
    armCount_ = new AllSpinBox;
    armCount_->setObjectName(QStringLiteral("armCount"));
    // guardado: 0 = todos. El rango (1..fijados) lo pone updateRaceControls
    const int saved = appSettings().value("analyzer/raceAutoPilots", 0).toInt();
    armAll_ = saved <= 0;
    armCount_->setRange(1, 16);
    armCount_->setValue(armAll_ ? 16 : saved);
    connect(armCount_, &QSpinBox::valueChanged, this, [this](int v) {
        armAll_ = v >= armCount_->maximum();
        appSettings().setValue("analyzer/raceAutoPilots", armAll_ ? 0 : v);
        updateRace();
    });
    arm->addWidget(armCount_);
    autoStop_ = new QCheckBox;
    autoStop_->setChecked(appSettings().value("analyzer/raceAutoStop", true).toBool());
    connect(autoStop_, &QCheckBox::toggled, this,
            [](bool on) { appSettings().setValue("analyzer/raceAutoStop", on); });
    arm->addSpacing(12);
    arm->addWidget(autoStop_);
    arm->addStretch(1);
    v->addLayout(arm);
    raceTable_ = new QTreeWidget;
    raceTable_->setRootIsDecorated(false);
    raceTable_->setColumnCount(8);
    raceTable_->header()->setStretchLastSection(true);
    raceTable_->setSelectionMode(QAbstractItemView::NoSelection);
    // clic derecho en una fila: aplicar solo la recomendacion de ese canal
    raceTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(raceTable_, &QWidget::customContextMenuRequested, this, &ChannelAnalyzer::raceTableMenu);
    v->addWidget(raceTable_, 1);
    auto *r = new QHBoxLayout;
    raceAdvice_ = new QLabel;
    raceAdvice_->setWordWrap(true);
    raceAdvice_->setTextFormat(Qt::RichText);
    r->addWidget(raceAdvice_, 1);
    applyBtn_ = new QPushButton;
    applyBtn_->setObjectName(QStringLiteral("apply"));
    applyBtn_->setVisible(false);
    connect(applyBtn_, &QPushButton::clicked, this, [this] { applyMoves(moves_); });
    r->addWidget(applyBtn_);
    v->addLayout(r);
    return page;
}

// clic derecho en una fila de la tabla de carrera: aplicar solo la recomendacion de ese canal, o todas
void ChannelAnalyzer::raceTableMenu(const QPoint &pos) {
    QTreeWidgetItem *it = raceTable_->itemAt(pos);
    if (!it) return;
    const int row = raceTable_->indexOfTopLevelItem(it);
    const auto ms = moves_;  // la tabla se rehace en cada barrido: lo de ahora
    const auto &hs = race_.health();
    QMenu menu(this);
    for (const auto &m : ms) {
        if (m.from != row || m.from >= static_cast<int>(hs.size())) continue;
        const QString t = QStringLiteral("%1 \u2192 %2").arg(QString::fromStdString(hs[m.from].ch.name),
                                                             QString::fromStdString(m.to.name));
        QAction *a = menu.addAction(canApplyAlone(m) ? tr("Apply %1 only").arg(t)
                                                     : tr("Apply %1 only (apply the others first)").arg(t));
        a->setEnabled(canApplyAlone(m));
        // con el menu abierto siguen llegando barridos: solo si sigue recomendada (y cabe sola)
        connect(a, &QAction::triggered, this, [this, m] {
            const auto v = stillValid({m});
            if (!v.empty() && canApplyAlone(v.front())) applyMoves(v);
        });
    }
    if (ms.size() > 1) {
        QAction *all = menu.addAction(tr("Apply all (%1)").arg(ms.size()));
        // todas o ninguna: cada una cuenta con las anteriores ya aplicadas
        connect(all, &QAction::triggered, this, [this, ms] {
            const auto v = stillValid(ms);
            if (v.size() == ms.size()) applyMoves(v);
        });
    }
    if (!menu.isEmpty()) menu.exec(raceTable_->viewport()->mapToGlobal(pos));
}

void ChannelAnalyzer::retranslate() {
    setWindowTitle(tr("5.8 GHz analyzer (video)"));
    bandsTitle_->setText(tr("Bands to analyse"));
    updateBandTexts();
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
    startBtn_->setText(session_.running() ? tr("Stop") : tr("Start analysis"));
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
                            tr("Pinned channel (right-click a mark to pin)"),
                            tr("Sweep: just measured (bright), fading until the next pass")};
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
    armBtn_->setText(tr("Start when the pilots are up"));
    armBtn_->setToolTip(tr("Starts the analysis if needed and waits: race tracking starts by itself as soon as the "
                           "chosen number of pilots are on the air on their pinned channels. Press again to cancel."));
    armLbl_->setText(tr("Pilots:"));
    static_cast<AllSpinBox *>(armCount_)->setAllText(tr("all"));
    armCount_->setToolTip(tr("How many pilots on their pinned channels start the race (all = every pinned channel)."));
    autoStop_->setText(tr("Stop when all have landed"));
    autoStop_->setToolTip(tr("Stops race tracking by itself when no pinned pilot has been seen for about a minute "
                             "and a half (VTXs off). A pilot that leaves its VTX on after landing keeps it going."));
    updateHistoryTab(history_->eventCount());
    applyBtn_->setText(tr("Apply"));
    applyBtn_->setToolTip(tr("Pins the new channel instead of the old one (tell the pilot to change it). The other "
                             "channels keep their history."));
    raceTable_->setHeaderLabels({tr("Channel"), tr("MHz"), tr("SNR (median / low)"), tr("Dropouts"), tr("Intruders"),
                                 tr("Interference"), tr("Quality"), tr("Recommendation")});
    raceTable_->headerItem()->setToolTip(7, tr("What to do with each channel: move it (→ new channel, tell the "
                                               "pilot and Apply), or why not. Right-click a row to apply only that "
                                               "one."));
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
    bool lostRun;
    if (!session_.setAdapterOpen(open, &lostRun)) return;
    startBtn_->setEnabled(open && haveChannels());
    if (lostRun) {
        showStopped(QString(), tr("The adapter was disconnected"));
        emit runningChanged(false);
    }
    if (!session_.running()) state_->setText(open ? tr("Ready: choose the bands and press Start analysis.")
                                                  : tr("Connect the adapter first."));
}

QString ChannelAnalyzer::bands() const {
    QString s;
    for (int i = 0; i < bandBoxes_.size(); i++)
        if (bandBoxes_[i]->isChecked()) s += QLatin1Char(kBands[i]);
    return s;
}

std::vector<advisor::Candidate> ChannelAnalyzer::analogCands() const {
    std::vector<advisor::Candidate> out;
    for (auto &c : advisor::analog_channels(bands().toStdString()))
        if (!excluded_.contains(QString::fromStdString(c.name))) out.push_back(c);
    return out;
}

std::vector<advisor::Candidate> ChannelAnalyzer::hdzCands() const {
    std::vector<advisor::Candidate> out;
    for (auto &c : advisor::hdzero_channels(bands().toStdString(), narrow_->isChecked()))
        if (!excluded_.contains(QString::fromStdString(c.name))) out.push_back(c);
    return out;
}

bool ChannelAnalyzer::haveChannels() const { return !analogCands().empty() || !hdzCands().empty(); }

void ChannelAnalyzer::bandsChanged() {
    appSettings().setValue("analyzer/bands", bands());
    appSettings().setValue("analyzer/excluded", excluded_);
    updateBandTexts();
    startBtn_->setEnabled(session_.adapterOpen() && haveChannels());
    if (session_.running()) {  // otra zona que barrer: se empieza de nuevo
        double lo, hi;
        range(lo, hi);
        if (haveChannels()) emit requestStart(lo, hi);
        else stop();
    }
    recompute();
    updateRace();
}

void ChannelAnalyzer::setExcluded(const QStringList &names, bool excluded) {
    double lo0, hi0, lo1, hi1;
    range(lo0, hi0);
    const QStringList list = names;  // puede ser excluded_ misma
    for (const QString &n : list) {
        excluded_.removeAll(n);
        if (excluded) excluded_ << n;
    }
    // una banda sin ningun canal queda desmarcada, con sus canales de vuelta: al marcarla otra vez, entera
    bool bandOff = false;
    for (int i = 0; i < bandBoxes_.size(); i++) {
        if (!bandBoxes_[i]->isChecked()) continue;
        QStringList chans;
        bool any = false;
        for (const auto &c : kp::fpv_channels())
            if (c.name[0] == kBands[i]) {
                chans << QString::fromLatin1(c.name);
                any = any || !excluded_.contains(chans.back());
            }
        if (any) continue;
        const QSignalBlocker b(bandBoxes_[i]);
        bandBoxes_[i]->setChecked(false);
        for (const QString &n : chans) excluded_.removeAll(n);
        bandOff = true;
    }
    range(lo1, hi1);
    if (bandOff || lo0 != lo1 || hi0 != hi1) {
        bandsChanged();
        return;
    }
    // la misma zona: no hace falta volver a barrer, solo cambian los candidatos
    appSettings().setValue("analyzer/excluded", excluded_);
    updateBandTexts();
    startBtn_->setEnabled(session_.adapterOpen() && haveChannels());
    recompute();
    updateRace();
}

void ChannelAnalyzer::updateBandTexts() {
    static const char *titles[] = {"A  Boscam A", "B  Boscam B", "E  Boscam E", "F  Fatshark", "R  Raceband", nullptr};
    for (int i = 0; i < bandBoxes_.size(); i++) {
        QString t = titles[i] ? QString::fromLatin1(titles[i]) : tr("L  Low band");
        int total = 0, on = 0;
        for (const auto &c : kp::fpv_channels())
            if (c.name[0] == kBands[i]) {
                total++;
                if (!excluded_.contains(QLatin1String(c.name))) on++;
            }
        if (on < total) t += QStringLiteral("  (%1/%2)").arg(on).arg(total);
        bandBoxes_[i]->setText(t);
        bandBtns_[i]->setEnabled(bandBoxes_[i]->isChecked());
        bandBtns_[i]->setToolTip(on < total ? tr("Channels of this band to analyse: %1 of %2").arg(on).arg(total)
                                            : tr("Choose which channels of this band to analyse"));
    }
}

void ChannelAnalyzer::showChannelMenu(int band, QToolButton *anchor) {
    // ventana emergente (no un QMenu, que se cierra a cada clic): una casilla por canal, Todos y Ninguno
    auto *pop = new QFrame(this, Qt::Popup);
    pop->setObjectName(QStringLiteral("channelMenu"));
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto *v = new QVBoxLayout(pop);
    v->setContentsMargins(8, 6, 8, 6);
    auto *grid = new QGridLayout;
    QStringList names;
    QVector<QCheckBox *> boxes;
    for (const auto &c : kp::fpv_channels()) {
        if (c.name[0] != kBands[band]) continue;
        const QString name = QString::fromLatin1(c.name);
        auto *cb = new QCheckBox(QStringLiteral("%1  %2 MHz").arg(name).arg(c.mhz, 0, 'f', 0));
        cb->setChecked(!excluded_.contains(name));
        connect(cb, &QCheckBox::toggled, this, [this, name, band, pop](bool on) {
            setExcluded({name}, !on);
            if (!bandBoxes_[band]->isChecked()) pop->close();  // ha quitado el ultimo: la banda se ha desmarcado
        });
        grid->addWidget(cb, boxes.size() % 4, boxes.size() / 4);  // dos columnas de cuatro
        names << name;
        boxes << cb;
    }
    v->addLayout(grid);
    auto *r = new QHBoxLayout;
    auto *all = new QPushButton(tr("All"));
    auto *none = new QPushButton(tr("None"));
    const auto setAll = [this, names, boxes, band, pop](bool on) {
        for (QCheckBox *cb : boxes) {
            const QSignalBlocker b(cb);
            cb->setChecked(on);
        }
        setExcluded(names, !on);
        if (!bandBoxes_[band]->isChecked()) pop->close();
    };
    connect(all, &QPushButton::clicked, this, [setAll] { setAll(true); });
    connect(none, &QPushButton::clicked, this, [setAll] { setAll(false); });
    r->addWidget(all);
    r->addWidget(none);
    v->addLayout(r);
    pop->adjustSize();
    pop->move(anchor->mapToGlobal(QPoint(anchor->width() - pop->width(), anchor->height())));
    pop->show();
}

void ChannelAnalyzer::range(double &lo, double &hi) const {
    lo = 1e9;
    hi = 0;
    // los canales que se analizan: quitar los de un extremo acorta el barrido
    std::vector<advisor::Candidate> cs = analogCands();
    for (const auto &c : hdzCands()) cs.push_back(c);
    for (const auto &c : cs) {
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
    if (session_.running()) {
        stop();
        return;
    }
    double lo, hi;
    range(lo, hi);
    last_ = SurveyData{};
    mine_.clear();  // "mio" vale para un analisis: otro dia, otro piloto puede estar en ese canal
    adviceLogKey_.clear();  // cada analisis empieza su recomendacion en el log
    wifiMem_.reset();
    session_.start();
    surveyT_ = session_.now();
    lastSweep_ = -1;
    updateAir();
    startBtn_->setText(tr("Stop"));
    state_->setText(tr("Analysing %1-%2 MHz...").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0));
    emit requestStart(lo, hi);
    emit runningChanged(true);
    recompute();
    updateRace();
}

void ChannelAnalyzer::stop() {
    if (!session_.stop()) return;
    showStopped(last_.sweeps > 0 ? tr("Stopped after %n sweep(s).", nullptr, last_.sweeps)
                                 : tr("Ready: choose the bands and press Start analysis."),
                tr("The analysis was stopped"));
    emit requestStop();  // su surveyStopped llega en cola: SurveySession lo descuenta
    emit runningChanged(false);
}

// El analisis se ha acabado (ya no esta en marcha): la ventana y la carrera. state vacio: el texto de estado se queda
void ChannelAnalyzer::showStopped(const QString &state, const QString &why) {
    startBtn_->setText(tr("Start analysis"));
    if (!state.isEmpty()) state_->setText(state);
    setArmed(false);
    setRace(false, why);
}

void ChannelAnalyzer::onStopped() {
    if (!session_.onStopped()) return;
    showStopped(tr("Stopped: the receiver went back to scanning or watching."),
                tr("The receiver went back to scanning or watching"));
    emit runningChanged(false);
}

void ChannelAnalyzer::abandon(const QString &why) {
    if (!session_.abandon()) return;
    showStopped(why, why);
    emit runningChanged(false);
}

void ChannelAnalyzer::updateAir() {
    // Se actualiza en el sitio: reconstruirla cada segundo perdia clics en la casilla y el desplazamiento
    QStringList keys, texts;
    QVector<bool> onPin;  // en un canal fijado: cuenta como mio sin marcarlo
    for (const auto &p : last_.pilots) {
        if (!onAir(p)) continue;
        const QString key = pilotKey(p);
        if (keys.contains(key)) continue;
        keys << key;
        onPin << pinnedMine(p);
        const bool leaving = onPin.back() && !onPinned(pinnedCandidates(), p);  // movido, aun en el canal viejo
        texts << QStringLiteral("%1  %2 MHz%3%4").arg(pilotShown(p)).arg(p.freqMHz, 0, 'f', 0)
                     .arg(p.isDigital() ? QStringLiteral("  ") + digitalTag(p, false) : QString(),
                          onPin.back() ? QStringLiteral("  (") + (leaving ? tr("changing channel") : tr("pinned")) +
                                             QLatin1Char(')')
                                       : QString());
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
    if (!session_.running()) return;
    // el barrido de 2,4 GHz (src/elrs_analyzer.h), de antes de empezar este: no es de aqui
    if (!d.bins.isEmpty() && d.bins.front().mhz < 3000) return;
    session_.onData();
    last_ = d;
    // el mismo nombre que en la ventana principal (y la clave de "mio" no salta entre R5 y A4 de un barrido a otro)
    for (Pilot &p : last_.pilots) chnames::apply(p);
    surveyT_ = session_.now();
    // el backend manda el espectro cada segundo, a medio barrido o no: lo que va por barrido, solo con uno nuevo
    const bool newSweep = d.sweeps != lastSweep_;
    lastSweep_ = d.sweeps;
    updateAir();
    if (newSweep) feedWifi();
    double lo, hi;
    range(lo, hi);
    state_->setText(d.sweeps == 0 ? tr("Analysing %1-%2 MHz: first sweep...").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0)
                                  // barrido dentro de la memoria (vuelve a 1 cada N) y el total, informativo
                                  : tr("Analysing %1-%2 MHz: sweep %3/%4 (%5 in total).")
                                        .arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0)
                                        .arg((d.sweeps - 1) % memory_->currentData().toInt() + 1)
                                        .arg(memory_->currentData().toInt()).arg(d.sweeps));
    const bool wasArmed = armed_, wasRace = raceOn_;
    if (armed_ && !raceOn_) checkArmed(raceSignals());
    // empezada ahora (auto-inicio): este barrido ya cuenta
    const bool raceSweep = raceOn_ && (newSweep || !wasRace);
    if (raceOn_) {
        const std::vector<advisor::Signal> air = raceSignals();
        if (raceSweep) {
            // el espectro tal cual (el monitor quita los fijados) y lo que esta en el aire
            std::vector<advisor::Bin> bins;
            bins.reserve(last_.bins.size());
            for (const auto &x : last_.bins) bins.push_back(advisor::Bin{x.mhz, x.meanDb, x.peakDb, x.duty});
            race_.update(bins, air, wifiMem_.known(surveyT_));
        }
        checkLanded(air);  // por tiempo: en cada actualizacion
    }
    // la recomendacion, con un barrido nuevo o si cambia lo que hay en el aire; si no, solo la grafica (el espectro
    // a medio barrido)
    if (newSweep || raceSweep || airKey() != airKey_ || wasRace != raceOn_) {
        recompute();
        updateRace();
    } else {
        double lo, hi;
        range(lo, hi);
        view_->setData(last_, advice_, lo, hi, mineForView());
        if (armed_ || wasArmed) updateRace();  // "esperando a los pilotos: x de y"
    }
}

QByteArray ChannelAnalyzer::airKey() const {
    // lo que hay en el aire tal como lo usan la recomendacion y el seguimiento (sin la edad, que sube cada segundo)
    QByteArray key;
    for (const Pilot &p : last_.pilots)
        if (onAir(p))
            key += pilotKey(p).toUtf8() + (p.isDigital() ? "d" : "a") + (p.ageS < kRaceHoldS ? "+" : "-") + ';';
    return key;
}

QStringList ChannelAnalyzer::mineForView() const {
    // mios para la grafica: los marcados y los que estan en un canal fijado
    QStringList mine = mine_;
    for (const Pilot &p : last_.pilots)
        if (onAir(p) && pinnedMine(p)) mine << pilotKey(p);
    return mine;
}

QString ChannelAnalyzer::noteText(const advisor::Pick &p, const std::vector<advisor::Wifi> &wifi) const {
    QStringList n;
    if (p.r.busy >= 0 && p.r.busy < seenIdx_.size()) {
        const Pilot &s = last_.pilots[seenIdx_[p.r.busy]];
        n << tr("already in use (%1)").arg(pilotShown(s));
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
    // y el que se ha movido (Aplicar) mientras sigue en su canal viejo: hasta que cambie, es suyo
    std::vector<advisor::Candidate> pins = pinnedCandidates();
    if (raceOn_)
        for (const auto &c : race_.leaving()) pins.push_back(c);
    std::vector<advisor::Signal> recent;  // todos: el nivel con que se tapa a uno mio no sale de un vecino
    for (const Pilot &p : last_.pilots)
        if (onAir(p)) recent.push_back(toSignal(p));
    if (all) *all = recent;
    for (int i = 0; i < last_.pilots.size(); i++) {
        const Pilot &p = last_.pilots[i];
        if (!onAir(p)) continue;
        const QString key = pilotKey(p);
        const advisor::Signal sig = toSignal(p);
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
    std::vector<advisor::Bin> bins;
    std::vector<advisor::Signal> seen;
    prepare(bins, seen);
    // las redes WiFi que recuerda este analisis (un AP callado se ve a ratos), no solo las del ultimo barrido
    const std::vector<advisor::Wifi> wifi = wifiMem_.known(surveyT_);
    const bool haveData = !bins.empty();
    QElapsedTimer t;
    t.start();
    const advisor::Advice a = haveData ? advisor::advise(bins, seen, analogCands(), hdzCands(), nAnalog_->value(),
                                                         nHdz_->value(), &wifi, pinnedCandidates())
                                       : pinnedOnlyAdvice();
    if (t.elapsed() > 100) KP_LOGW("analizador: recomendar ha tardado %lld ms", static_cast<long long>(t.elapsed()));
    recomputes_++;
    advice_ = a;
    airKey_ = airKey();
    double lo, hi;
    range(lo, hi);
    view_->setData(last_, a, lo, hi, mineForView());

    const QSignalBlocker block(table_);  // las casillas de fijar: solo los clics del usuario
    table_->clear();
    if (!haveData) {
        showUnmeasured(a);
        return;
    }
    // los marcados como mios (sin canal fijado) se tratan como si no estuvieran: su canal queda libre y se reparten
    // con los demas. Se dice en que canal recomendado estan ya y cuales tienen que cambiar (o fijarse si se quedan)
    QStringList mineShown;
    std::vector<advisor::Signal> mine;
    for (const Pilot &p : last_.pilots) {
        if (!onAir(p) || pinnedMine(p) || !mine_.contains(pilotKey(p))) continue;
        mineShown << pilotShown(p);
        mine.push_back(toSignal(p));
    }
    const std::vector<std::vector<int>> on = race::already_on(a.picks, mine);
    QStringList toMove;  // los mios que no estan en ningun recomendado
    for (int k = 0; k < mineShown.size(); k++)
        if (std::none_of(on.begin(), on.end(), [k](const std::vector<int> &v) {
                return std::find(v.begin(), v.end(), k) != v.end();
            }))
            toMove << mineShown[k];
    for (size_t i = 0; i < a.picks.size(); i++) {
        QStringList here;
        for (int k : on[i]) here << mineShown[k];
        addAdviceRow(a.picks[i], a.wifi, here);
    }
    for (int c = 0; c < 6; c++) table_->resizeColumnToContents(c);
    summary_->setText(summaryText(a, toMove));
    logAdvice(a, toMove);
}

advisor::Advice ChannelAnalyzer::pinnedOnlyAdvice() const {
    // sin barridos todavia (recien abierta): los fijados se ven igual, en la grafica y en la tabla, sin medir
    advisor::Advice a;
    for (const auto &c : pinnedCandidates()) {
        advisor::Pick pk;
        pk.r.ch = c;
        pk.pinned = true;
        a.picks.push_back(pk);
    }
    return a;
}

namespace {
QString pinLabel(const QString &name, bool hdz) {
    return QStringLiteral("<b>%1</b>%2").arg(name, hdz ? QStringLiteral(" (HDZero)") : QString());
}
}  // namespace

QTreeWidgetItem *ChannelAnalyzer::addPickRow(const advisor::Pick &p) {
    const QString name = QString::fromStdString(p.r.ch.name);
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
    if (p.pinned) {
        QFont bold = table_->font();
        bold.setBold(true);
        it->setFont(2, bold);
    }
    it->setText(3, QString::number(p.r.ch.mhz, 'f', 0));
    return it;
}

void ChannelAnalyzer::showUnmeasured(const advisor::Advice &a) {
    QString s = session_.running() ? tr("Waiting for the first sweep...")
                                   : tr("Choose the bands and how many pilots will fly, then start the analysis.");
    if (!pinned_.isEmpty()) {
        QStringList pins;
        for (const Pin &p : pinned_) pins << pinLabel(p.name, p.hdzero);
        s += QStringLiteral(" ") + tr("Pinned: %1.").arg(pins.join(QStringLiteral(", ")));
    }
    summary_->setText(s);
    for (const auto &p : a.picks) {
        QTreeWidgetItem *it = addPickRow(p);
        it->setText(4, QStringLiteral("-"));
        it->setText(5, QStringLiteral("-"));
        it->setText(6, session_.running() ? tr("not measured yet") : tr("not measured yet: start the analysis"));
    }
    for (int c = 0; c < 6; c++) table_->resizeColumnToContents(c);
}

void ChannelAnalyzer::addAdviceRow(const advisor::Pick &p, const std::vector<advisor::Wifi> &wifi,
                                   const QStringList &mineHere) {
    QTreeWidgetItem *it = addPickRow(p);
    it->setText(4, QStringLiteral("%1 dB").arg(p.r.noise_db, 0, 'f', 1));
    it->setText(5, QStringLiteral("%1 %").arg(qRound(p.r.burst * 100)));
    QStringList note;
    if (const QString n = noteText(p, wifi); !n.isEmpty()) note << n;
    if (p.pinned)
        // se da por mio sin preguntar (los fijados se guardan: otro dia puede ser otro piloto): que se vea
        for (const Pilot &pl : last_.pilots)
            if (onAir(pl) && onPinned({p.r.ch}, pl)) {
                note << tr("%1 is on the air here: taken as yours (if it is not, unpin this channel)")
                            .arg(pilotShown(pl));
                break;
            }
    for (const QString &m : mineHere) note << tr("your pilot %1 is already here").arg(m);
    it->setText(6, note.join(QStringLiteral("; ")));
    it->setForeground(4, ratingColour(p.r));
}

QString ChannelAnalyzer::summaryText(const advisor::Advice &a, const QStringList &toMove) const {
    QStringList names, pinnedNames;
    for (const auto &p : a.picks)
        (p.pinned ? pinnedNames : names) << pinLabel(QString::fromStdString(p.r.ch.name), p.r.ch.hdzero);
    // el aviso de interferencia, por los que elige la recomendacion (lo de los fijados ya se ve en la tabla)
    float worstSuggested = 0;
    for (const auto &p : a.picks)
        if (!p.pinned) worstSuggested = std::max(worstSuggested, p.r.noise_db);
    const auto span = [](const char *colour, const QString &text) {
        return QStringLiteral(" <span style='color:%1'>").arg(QLatin1String(colour)) + text +
               QStringLiteral("</span>");
    };
    QString s;
    if (a.picks.empty()) s = tr("Set how many pilots will fly.");
    else if (pinnedNames.isEmpty()) s = tr("Recommended: %1.").arg(names.join(QStringLiteral(", ")));
    else if (names.isEmpty()) s = tr("Pinned: %1.").arg(pinnedNames.join(QStringLiteral(", ")));
    else s = tr("Pinned: %1. Suggested for the rest: %2.").arg(pinnedNames.join(QStringLiteral(", ")),
                                                                  names.join(QStringLiteral(", ")));
    if (!a.complete)
        s += span("#d9483b", pinnedNames.isEmpty()
                                 ? tr("Not enough room for that many pilots in these bands with safe spacing: add "
                                      "bands or fly fewer at once.")
                                 : tr("Not enough room for that many pilots with safe spacing next to the pinned "
                                      "channels: unpin some, add bands or fly fewer at once."));
    else if (worstSuggested >= 8)
        s += span("#e0a020", tr("Even the best channels have noticeable interference here."));
    if (!a.wifi.empty())
        s += QStringLiteral(" ") + tr("WiFi seen on channel(s) %1: it counts against the channels it covers, "
                                      "even while quiet.").arg(wifiChannels(a.wifi, QStringLiteral("\u2013"))
                                                                   .join(QStringLiteral(", ")));
    if (!toMove.isEmpty() && !a.picks.empty()) {
        QStringList bold;
        for (const QString &m : toMove) bold << QStringLiteral("<b>%1</b>").arg(m);
        s += span("#e0a020",
                  (toMove.size() == 1
                       ? tr("%1 must change to one of the suggested channels (or pin its channel if it stays).")
                       : tr("%1 must change to the suggested channels (or pin their channels if they stay)."))
                      .arg(bold.join(QStringLiteral(", "))));
    }
    if (last_.sweeps < 3) s += QStringLiteral(" ") + tr("(still learning: let it run a few sweeps)");
    return s;
}

QStringList ChannelAnalyzer::wifiChannels(const std::vector<advisor::Wifi> &wifi, const QString &dash) {
    QStringList chs;
    for (const auto &g : advisor::group_wifi(wifi))
        chs << (g.first == g.last ? QString::number(g.first) : QString::number(g.first) + dash + QString::number(g.last));
    return chs;
}

void ChannelAnalyzer::logAdvice(const advisor::Advice &a, const QStringList &moving) {
    // al log, cuando cambia lo recomendado (canales, notas, WiFi, quien tiene que cambiar), no en cada barrido: los
    // dB van en la linea pero no cuentan como cambio
    if (!session_.running() || last_.sweeps < 3 || a.picks.empty()) return;
    QStringList key, line;
    for (const auto &p : a.picks) {
        const QString name = chName(p.r.ch);
        const QString note = noteText(p, a.wifi);
        // la clave, con lo que se recomienda y no con su texto: el de una red WiFi cambia casi en cada barrido
        // ("ahora callada" / "vista hace poco" / "en uso") sin que cambie nada de lo recomendado
        QString k = name + (p.pinned ? QStringLiteral("*") : QString());
        for (auto n : p.notes) k += QLatin1Char('n') + QString::number(static_cast<int>(n));
        if (p.r.busy >= 0) k += QStringLiteral("b");
        if (p.r.burst > 0.1f) k += QStringLiteral("r");
        if (p.r.wifi >= 0 && p.r.wifi < static_cast<int>(a.wifi.size()))
            k += QStringLiteral("w%1%2").arg(a.wifi[p.r.wifi].ch).arg(p.r.wifi_block ? "B" : "");
        key << k;
        line << QStringLiteral("%1 %2 MHz%3: %4 dB, rafagas %5 %%6")
                    .arg(name)
                    .arg(p.r.ch.mhz, 0, 'f', 0)
                    .arg(p.pinned ? QStringLiteral(" (fijado)") : QString())
                    .arg(p.r.noise_db, 0, 'f', 1)
                    .arg(qRound(p.r.burst * 100))
                    .arg(note.isEmpty() ? QString() : QStringLiteral(" (") + note + QLatin1Char(')'));
    }
    const QStringList wifiChs = wifiChannels(a.wifi, QStringLiteral("-"));
    key << wifiChs.join(QLatin1Char(',')) << moving.join(QLatin1Char(',')) << (a.complete ? "1" : "0");
    const QString k = key.join(QLatin1Char(';'));
    if (k == adviceLogKey_) return;
    adviceLogKey_ = k;
    QString msg = line.join(QStringLiteral("; "));
    if (!wifiChs.isEmpty()) msg += QStringLiteral(" | WiFi: ") + wifiChs.join(QStringLiteral(", "));
    if (!moving.isEmpty()) msg += QStringLiteral(" | tienen que cambiar: ") + moving.join(QStringLiteral(", "));
    if (!a.complete) msg += QStringLiteral(" | no caben con separacion segura");
    KP_LOGI("analizador: recomendacion (barrido %d): %s", last_.sweeps, qUtf8Printable(msg));
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

QString ChannelAnalyzer::pinsText() const {
    QStringList l;
    for (const Pin &p : pinned_) l << p.name + (p.hdzero ? QStringLiteral(" HDZero") : QString());
    return l.join(QStringLiteral(", "));
}

bool ChannelAnalyzer::isPinned(const QString &name, bool hdzero) const {
    for (const Pin &p : pinned_)
        if (p.name == name && p.hdzero == hdzero) return true;
    return false;
}

void ChannelAnalyzer::savePins() const {
    QStringList l;
    for (const Pin &p : pinned_) l << (p.hdzero ? QStringLiteral("hdz:") : QString()) + p.name;
    appSettings().setValue("analyzer/pinned", l);
}

void ChannelAnalyzer::setPins(const QVector<Pin> &pins) {
    pinned_ = pins;
    savePins();
    moves_.clear();
    pinLastT_.clear();  // otros fijados: lo de cada uno vuelve a empezar
    noBetter_.clear();
    // cambio a mano: el seguimiento vuelve a empezar (sin fijados, se apaga); parado, lo de la ultima carrera ya
    // no casa con los fijados
    if (raceOn_ && !pinned_.isEmpty()) {
        // otros pilotos: lo de hasta ahora se cierra con su resumen y sigue como otra carrera
        addSummary();
        race_.start(pinnedCandidates());
        beginRaceRec();
        addEvent(Ev::Start, QString(), tr("Pinned channels changed by hand"),
                 tr("Tracking starts again with %1.").arg(pinsText()));
    } else if (raceOn_) setRace(false, tr("No pinned channels left"));
    else race_.start({});
    if (pinned_.isEmpty()) setArmed(false);
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

bool ChannelAnalyzer::pinnedMine(const Pilot &p) const {
    // el canal que deja uno movido solo mientras se sigue la carrera (si no, nadie lo olvida: update no corre)
    return onPinned(pinnedCandidates(), p) || (raceOn_ && onPinned(race_.leaving(), p));
}

// ---- seguimiento de carrera ---------------------------------------------------------------------------------

void ChannelAnalyzer::setRace(bool on, const QString &why) {
    on = on && session_.running() && !pinned_.isEmpty();
    if (raceBox_->isChecked() != on) {
        const QSignalBlocker b(raceBox_);
        raceBox_->setChecked(on);
    }
    if (on != raceOn_) {
        raceOn_ = on;
        moves_.clear();
        raceLogKeys_.clear();
        noBetter_.clear();
        const QString pins = pinsText();
        const QString how = why.isEmpty() ? tr("By hand") : why;
        if (on) {
            armed_ = false;  // empezada (a mano o sola): ya no se espera a nadie
            race_.start(pinnedCandidates());  // apagado: lo apuntado se queda a la vista
            beginRaceRec();
            raceSeenAny_ = false;
            lastPresentT_ = surveyT_;
            pinLastT_.clear();
            lingerWarned_ = false;
            KP_LOGI("carrera: seguimiento en marcha (%s; fijados: %s)", qUtf8Printable(how),
                    qUtf8Printable(pins));
            addEvent(Ev::Start, QString(), tr("Race started"),
                     tr("%1. Pinned: %2.").arg(how, pins));
        } else {
            KP_LOGI("carrera: seguimiento parado tras %d barrido(s) (%s)", race_.sweeps(), qUtf8Printable(how));
            addSummary();
            addEvent(Ev::Info, QString(), tr("Race stopped"),
                     tr("%1, after %n sweep(s).", nullptr, race_.sweeps()).arg(how));
        }
        updateAir();
        recompute();
    }
    updateRace();
}

void ChannelAnalyzer::updateRaceControls() {
    raceBox_->setEnabled(session_.running() && !pinned_.isEmpty());
    unpinBtn_->setEnabled(!pinned_.isEmpty());
    // armar: con fijados y sin carrera en marcha; si el analisis no esta en marcha, lo arranca (hace falta adaptador)
    const bool canStart = session_.running() || (session_.adapterOpen() && haveChannels());
    armBtn_->setEnabled(armed_ || (!raceOn_ && !pinned_.isEmpty() && canStart));
    if (armBtn_->isChecked() != armed_) {
        const QSignalBlocker b(armBtn_);
        armBtn_->setChecked(armed_);
    }
    {
        // "todos" sigue siendo todos con mas o menos fijados; un numero se queda (o baja al maximo)
        const QSignalBlocker b(armCount_);
        const bool all = armAll_;
        armCount_->setMaximum(std::max(1, static_cast<int>(pinned_.size())));
        if (all) armCount_->setValue(armCount_->maximum());
        armAll_ = all || armCount_->value() >= armCount_->maximum();
        auto *sb = static_cast<AllSpinBox *>(armCount_);
        sb->setAllText(sb->allText);  // al cambiar el maximo, el mismo valor puede pasar a ser "todos"
    }
}

std::vector<advisor::Signal> ChannelAnalyzer::raceSignals() const {
    // visto hace menos de kRaceHoldS: los pilotos se miden menos a menudo que el espectro
    std::vector<advisor::Signal> now;
    for (const Pilot &p : last_.pilots)
        if (p.ageS < kRaceHoldS)
            now.push_back(toSignal(p));
    return now;
}

// ---- auto-inicio y fin automatico --------------------------------------------------------------------------

void ChannelAnalyzer::setArmed(bool on) {
    on = on && !pinned_.isEmpty() && !raceOn_;
    if (on && !session_.running()) {
        if (session_.adapterOpen() && haveChannels()) toggle();  // arranca el analisis: sin barridos no se ve a nadie
        on = session_.running();
    }
    if (on != armed_) {
        armed_ = on;
        armPresent_ = 0;
        const int n = static_cast<int>(pinned_.size());
        const int want = armWant();
        if (on) KP_LOGI("carrera: esperando a %d de %d piloto(s) para empezar", want, n);
        else KP_LOGI("carrera: ya no se espera a los pilotos");
        if (on && session_.running() && last_.sweeps > 0) checkArmed(raceSignals());  // ya pueden estar todos
    }
    updateRace();
}

int ChannelAnalyzer::armWant() const {
    const int n = static_cast<int>(pinned_.size());
    return armAll_ ? n : std::min(armCount_->value(), n);
}

void ChannelAnalyzer::checkArmed(const std::vector<advisor::Signal> &now) {
    const int n = static_cast<int>(pinned_.size());
    if (!armed_ || n == 0) return;
    const int want = armWant();
    armPresent_ = race::present(pinnedCandidates(), now);
    if (armPresent_ >= want)
        setRace(true, tr("Started by itself: %1 of %2 pilot(s) on their channels").arg(armPresent_).arg(n));
}

void ChannelAnalyzer::checkLanded(const std::vector<advisor::Signal> &now) {
    const std::vector<advisor::Candidate> pinned = pinnedCandidates();
    std::vector<advisor::Candidate> pins = pinned;
    for (const auto &c : race_.leaving()) pins.push_back(c);
    // por fijado, cuando se le vio por ultima vez (para avisar si uno solo impide el fin automatico)
    const int n = static_cast<int>(pinned.size());
    if (pinLastT_.size() != n) pinLastT_ = QVector<double>(n, -1);
    QVector<bool> here(n, false);
    for (int i = 0; i < n; i++)
        if (race::present({pinned[i]}, now) > 0) {
            here[i] = true;
            pinLastT_[i] = surveyT_;
        }
    if (race::present(pins, now) > 0) {
        lastPresentT_ = surveyT_;
        raceSeenAny_ = true;
        // Cualquier senal del mismo sistema a menos de race::kMatchMhz de un fijado cuenta como su piloto: un VTX
        // ajeno (o encendido en la mesa) junto a un canal fijado impide que pare sola. Si los demas ya han
        // aterrizado (vistos en esta carrera y no desde hace kLandedS) y quedan solo unos pocos, se avisa una vez.
        QStringList still;
        int landed = 0, stillIdx = -1;
        for (int i = 0; i < n; i++) {
            if (here[i]) {
                still << chName(pinned[i]);
                stillIdx = i;
            }
            else if (pinLastT_[i] >= 0 && surveyT_ - pinLastT_[i] >= kLandedS) landed++;
        }
        if (autoStop_->isChecked() && !lingerWarned_ && !still.isEmpty() && landed > 0 &&
            still.size() + landed == n && still.size() < n) {
            lingerWarned_ = true;
            const QString l = still.join(QStringLiteral(", "));
            KP_LOGI("carrera: solo queda en el aire %s; no para sola mientras siga", qUtf8Printable(l));
            addEvent(Ev::Warn, l, tr("Still on the air: the race will not stop by itself"),
                     tr("The other pilots are no longer seen. If this is not one of your pilots (someone else's VTX "
                        "near its channel), stop the race by hand."),
                     still.size() == 1 ? stillIdx : -1);
        }
        return;
    }
    // solo si ya ha salido alguno: recien empezada a mano puede que aun no haya despegado nadie
    if (autoStop_->isChecked() && raceSeenAny_ && surveyT_ - lastPresentT_ >= kLandedS)
        setRace(false, tr("Stopped by itself: no pilot on the air for over a minute"));
}

// ---- historial (src/race_history.h) --------------------------------------------------------------------------

void ChannelAnalyzer::addEvent(Ev kind, const QString &channel, const QString &what, const QString &detail,
                               int pilot) {
    history_->addEvent(kind, race_.sweeps(), channel, what, detail, pilot);
}

void ChannelAnalyzer::beginRaceRec() {
    QStringList channels;
    for (const auto &h : race_.health()) channels << chName(h.ch);
    history_->beginRace(race_.health(), channels);
}

void ChannelAnalyzer::addSummary() {
    const auto &hs = race_.health();
    if (race_.sweeps() == 0) return;
    history_->syncHealth(hs);
    const int worst = race_.worst();
    for (int i = 0; i < static_cast<int>(hs.size()); i++)
        addEvent(Ev::Summary, chName(hs[i].ch),
                 i == worst && hs.size() > 1 ? tr("Summary (worst video)") : tr("Summary"),
                 RaceHistory::healthText(hs[i], true), i);
}

void ChannelAnalyzer::updateHistoryTab(int events) {
    tabs_->setTabText(2, events == 0 ? tr("History") : tr("History (%1)").arg(events));
}

void ChannelAnalyzer::rowAdvice(int i, QString &rec, QString &tip, QColor &col, bool &noBetter) const {
    // recomendacion de este canal, de un vistazo: "-> F3", o por que no se mueve
    const race::ChannelHealth &h = race_.health()[i];
    const QColor red(0xd9, 0x48, 0x3b), orange(0xe0, 0xa0, 0x20), green(0x3a, 0xa8, 0x5a);
    rec.clear();
    tip.clear();
    col = QColor();
    noBetter = false;
    for (const auto &m : moves_) {
        if (m.from != i) continue;
        rec = QStringLiteral("→ %1  (%2 MHz, +%3 dB)")
                  .arg(QString::fromStdString(m.to.name))
                  .arg(m.to.mhz, 0, 'f', 0)
                  .arg(m.gain_db, 0, 'f', 0);
        tip = (m.cause == race::Cause::Intruder ? tr("Someone else is transmitting on top of it.")
                                                : tr("Interference or WiFi on its channel.")) +
              QLatin1Char(' ') + tr("Tell the pilot, then Apply (right-click: only this one).");
        col = red;
        return;
    }
    if (!raceOn_ || h.seen == 0 || h.sweeps < race::kMinSweeps) return;
    // con el mismo margen que las recomendaciones (race::kMoveKeepGainDb): si ya lo decia, sigue mientras no baje de
    // ahi; si no, no alterna con OK por unas decimas
    const bool was = i < noBetter_.size() && noBetter_[i];
    if (h.penalty >= (was ? race::kMoveKeepGainDb : race::kMoveMinGainDb)) {
        // su canal es el problema, pero no hay otro libre compatible claramente mejor
        noBetter = true;
        rec = tr("No better free channel");
        tip = (5.0 * h.intruders > h.sweeps ? tr("Someone else is transmitting on top of it.")
                                            : tr("Interference or WiFi on its channel.")) +
              QLatin1Char(' ') +
              tr("No free channel compatible with the other pinned ones would be clearly better: unpin one "
                 "or add bands.");
        col = orange;
    } else if (h.snr_low < kLowSnrDb || h.lost > 0) {
        rec = tr("Weak video: not the channel");
        tip = tr("Weak video on a clean channel: changing channel would not fix it. Check its antennas, VTX "
                 "power and distance.");
        col = orange;
    } else {
        rec = tr("OK");
        col = green;
    }
}

void ChannelAnalyzer::logRaceRow(int i, const QString &rec, const QString &tip, const QColor &col) {
    // al log y al Historial: el canal y su recomendacion, solo cuando cambia de tipo (OK, mover, sin canal mejor,
    // video flojo) y su causa; el destino de un movimiento va en la linea, pero no cuenta: puede ir cambiando entre
    // canales casi iguales sin que cambie lo que hay que hacer
    const race::ChannelHealth &h = race_.health()[i];
    const race::Move *mv = nullptr;
    for (const auto &m : moves_)
        if (m.from == i) mv = &m;
    const QString key = chName(h.ch) + QLatin1Char('|') +
                        (mv ? QStringLiteral("move%1").arg(static_cast<int>(mv->cause)) : rec);
    while (raceLogKeys_.size() <= i) raceLogKeys_ << QString();
    if (!raceOn_ || race_.sweeps() < race::kMinSweeps) return;
    const bool changed = !rec.isEmpty() && raceLogKeys_[i] != key;
    raceLogKeys_[i] = key;
    if (!changed) return;
    const QString seenTxt = h.seen > 0 ? QStringLiteral("SNR %1/%2 dB, calidad %3 dB")
                                             .arg(h.snr_med, 0, 'f', 0)
                                             .arg(h.snr_low, 0, 'f', 0)
                                             .arg(h.quality, 0, 'f', 0)
                                       : QStringLiteral("piloto no visto");
    KP_LOGI("carrera: %s %.0f MHz: %s%s%s [%s, cortes %d, intrusos %d %%, interferencia %.1f dB, %d barridos]",
            qUtf8Printable(chName(h.ch)), h.ch.mhz, qUtf8Printable(rec), tip.isEmpty() ? "" : " - ",
            qUtf8Printable(tip), qUtf8Printable(seenTxt), h.lost,
            h.sweeps > 0 ? qRound(100.0 * h.intruders / h.sweeps) : 0, h.channel_cost, h.sweeps);
    const QString stats = RaceHistory::healthText(h, false);
    const bool noBetter = i < noBetter_.size() && noBetter_[i];
    addEvent(mv ? Ev::Move : col == QColor(0x3a, 0xa8, 0x5a) ? Ev::Ok : noBetter ? Ev::Problem : Ev::Warn,
             chName(h.ch), rec, tip.isEmpty() ? stats : tip + QLatin1Char(' ') + stats, i);
}

QString ChannelAnalyzer::raceStateText() const {
    const int n = static_cast<int>(race_.health().size()), sw = race_.sweeps();
    if (pinned_.isEmpty()) return tr("Pin the channels you have given to the pilots first (Recommendation tab).");
    if (armed_)
        return tr("Waiting for the pilots: %1 of %2 on their pinned channels. Tracking starts by itself.")
            .arg(armPresent_).arg(armWant());
    if (!raceOn_ && sw > 0) return tr("Tracking stopped after %n sweep(s).", nullptr, sw);
    if (!raceOn_)
        return session_.running() ? tr("Tick it when the race starts.")
                        : tr("Start the analysis, then tick it when the race starts.");
    if (sw < race::kMinSweeps) return tr("Learning the channels: sweep %1 of %2...").arg(sw).arg(race::kMinSweeps);
    return tr("Following %n pinned channel(s): %1 sweeps.", nullptr, n).arg(sw);
}

void ChannelAnalyzer::refreshMoves() {
    // las recomendaciones (una por canal): las que se estan mostrando se mantienen mientras sigan valiendo (no
    // cambian de canal por poco entre barridos)
    const std::vector<race::Move> prev = moves_;
    moves_.clear();
    if (!raceOn_) return;
    const int n = static_cast<int>(race_.health().size());
    for (const auto &m : race_.suggest_all(analogCands(), hdzCands(), prev))
        if (m.from >= 0 && m.from < n && m.from < pinned_.size()) moves_.push_back(m);
}

bool ChannelAnalyzer::addRaceRow(int i, bool worst, const QFont &bold) {
    const race::ChannelHealth &h = race_.health()[i];
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
    QString rec, tip;
    QColor col;
    bool noBetter = false;
    rowAdvice(i, rec, tip, col, noBetter);
    if (noBetter_.size() <= i) noBetter_.resize(static_cast<int>(race_.health().size()));
    noBetter_[i] = noBetter;
    logRaceRow(i, rec, tip, col);
    it->setText(7, rec);
    if (!tip.isEmpty()) it->setToolTip(7, tip);
    if (col.isValid()) it->setForeground(7, col);
    it->setFont(7, bold);
    if (worst) {
        for (int c = 0; c < raceTable_->columnCount(); c++) {
            it->setBackground(c, QColor(0xd9, 0x48, 0x3b, 50));
            if (c != 7) it->setFont(c, bold);
        }
        it->setToolTip(0, tr("The worst video so far"));
    }
    return rec.isEmpty() || col == QColor(0x3a, 0xa8, 0x5a);  // OK (verde) o sin nada que decir
}

QString ChannelAnalyzer::raceAdviceText(bool allOk) const {
    // los cambios a hacer (todos a la vez son compatibles) o que no hace falta ninguno
    if (!moves_.empty()) {
        QStringList l;
        for (const auto &m : moves_)
            l << QStringLiteral("<b>%1</b> → <b>%2</b>").arg(chName(race_.health()[m.from].ch), chName(m.to));
        return tr("Change: %1. Tell the pilots, then Apply.").arg(l.join(QStringLiteral(", ")));
    }
    if (raceOn_ && race_.sweeps() >= race::kMinSweeps && race_.worst() >= 0 && allOk)
        return tr("No change needed: no pilot would clearly do better on another free channel.");
    return QString();
}

void ChannelAnalyzer::updateRace() {
    if (!raceTable_) return;
    updateRaceControls();
    refreshMoves();
    raceTable_->clear();
    QFont bold = raceTable_->font();
    bold.setBold(true);
    const int n = static_cast<int>(race_.health().size()), worst = race_.worst();
    bool allOk = true;
    for (int i = 0; i < n; i++) allOk = addRaceRow(i, i == worst && n > 1, bold) && allOk;
    for (int c = 0; c < 7; c++) raceTable_->resizeColumnToContents(c);
    raceState_->setText(raceStateText());
    raceAdvice_->setText(raceAdviceText(allOk));
    if (raceOn_) {
        history_->syncHealth(race_.health());
        if (tabs_->currentIndex() == 2) history_->refreshPilots();  // su video, en vivo
    }
    raceAdvice_->setStyleSheet(moves_.empty() ? QString() : QStringLiteral("color:#d9483b;"));
    applyBtn_->setText(moves_.size() > 1 ? tr("Apply all (%1)").arg(moves_.size()) : tr("Apply"));
    applyBtn_->setVisible(!moves_.empty());
}

std::vector<race::Move> ChannelAnalyzer::stillValid(const std::vector<race::Move> &moves) const {
    std::vector<race::Move> out;
    if (!raceOn_) return out;
    for (const auto &m : moves)
        for (const auto &cur : moves_)
            if (cur.from == m.from && cur.to.name == m.to.name && cur.to.hdzero == m.to.hdzero) {
                out.push_back(cur);
                break;
            }
    return out;
}

bool ChannelAnalyzer::canApplyAlone(const race::Move &m) const {
    // calculadas una tras otra, una recomendacion cuenta con las anteriores ya aplicadas: sola, tiene que caber con
    // los fijados de ahora
    const auto &hs = race_.health();
    for (int j = 0; j < static_cast<int>(hs.size()); j++)
        if (j != m.from && std::abs(hs[j].ch.mhz - m.to.mhz) < advisor::min_spacing(hs[j].ch, m.to)) return false;
    return true;
}

void ChannelAnalyzer::applyMoves(const std::vector<race::Move> &moves) {
    bool any = false;
    for (const auto &m : moves) {
        if (m.from < 0 || m.from >= pinned_.size() || m.from >= static_cast<int>(race_.health().size())) continue;
        // solo cambia ese fijado: lo de los demas sigue (moved, no start)
        const advisor::Candidate &old = race_.health()[m.from].ch;
        if (raceOn_) history_->pilotMoved(m.from, chName(m.to));
        addEvent(Ev::Applied, chName(old),
                 tr("Moved to %1 (%2 MHz)").arg(QString::fromStdString(m.to.name)).arg(m.to.mhz, 0, 'f', 0),
                 tr("Applied: its tracking starts again on the new channel."), m.from);
        pinned_[m.from] = Pin{QString::fromStdString(m.to.name), m.to.hdzero};
        race_.moved(m.from, m.to);
        any = true;
    }
    if (!any) return;
    moves_.clear();
    savePins();
    updateAir();
    recompute();
    updateRace();
}
