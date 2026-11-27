#include "race_history.h"
#include "analyzer_style.h"
#include "log.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {
QString raceTime(double s) {
    const int t = std::max(0, static_cast<int>(s));
    const QLatin1Char zero('0');
    return t >= 3600 ? QStringLiteral("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, zero).arg(t % 60, 2, 10, zero)
                     : QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, zero);
}

QColor catColour(int c) {  // Cat: graves, avisos, cambios, informacion
    static const QColor cs[] = {QColor(0xd9, 0x48, 0x3b), QColor(0xe0, 0xa0, 0x20), QColor(0x2f, 0x80, 0xed),
                                QColor(0x80, 0x80, 0x80)};
    return cs[std::clamp(c, 0, 3)];
}
}  // namespace

RaceHistory::Cat RaceHistory::category(Ev k) {
    switch (k) {
    case Ev::Problem:
    case Ev::Move: return Cat::Serious;
    case Ev::Warn: return Cat::Warning;
    case Ev::Applied: return Cat::Change;
    case Ev::Info:
    case Ev::Start:
    case Ev::Ok:
    case Ev::Summary: break;
    }
    return Cat::Info;
}

RaceHistory::RaceHistory(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto *top = new QHBoxLayout;
    histRaceLbl_ = new QLabel;
    top->addWidget(histRaceLbl_);
    histRace_ = new QComboBox;
    histRace_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(histRace_, &QComboBox::activated, this, [this] {
        pilotPicked_ = false;
        rebuild();
    });
    top->addWidget(histRace_);
    histView_ = new QComboBox;
    histView_->setObjectName(QStringLiteral("historyView"));
    histView_->addItem(QString());  // por piloto
    histView_->addItem(QString());  // cronologico
    connect(histView_, &QComboBox::currentIndexChanged, this, [this](int i) { histStack_->setCurrentIndex(i); });
    top->addWidget(histView_);
    histVerdict_ = new QLabel;
    histVerdict_->setObjectName(QStringLiteral("historyVerdict"));
    histVerdict_->setWordWrap(true);
    histVerdict_->setTextFormat(Qt::RichText);
    top->addWidget(histVerdict_, 1);
    v->addLayout(top);
    histStack_ = new QStackedWidget;
    {
        auto *split = new QSplitter(Qt::Horizontal);
        pilotTable_ = new QTreeWidget;
        pilotTable_->setObjectName(QStringLiteral("historyPilots"));
        pilotTable_->setRootIsDecorated(false);
        pilotTable_->setColumnCount(7);
        pilotTable_->header()->setStretchLastSection(false);
        // rebuildPilotView bloquea las senales: lo que llega aqui lo ha elegido el usuario
        connect(pilotTable_, &QTreeWidget::currentItemChanged, this, [this] {
            pilotPicked_ = true;
            showPilotEvents();
        });
        split->addWidget(pilotTable_);
        auto *right = new QWidget;
        auto *rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        pilotInfo_ = new QLabel;
        pilotInfo_->setObjectName(QStringLiteral("historyPilotInfo"));
        pilotInfo_->setWordWrap(true);
        pilotInfo_->setTextFormat(Qt::RichText);
        rv->addWidget(pilotInfo_);
        pilotEvents_ = new QTreeWidget;
        pilotEvents_->setObjectName(QStringLiteral("historyEvents"));
        pilotEvents_->setColumnCount(3);
        pilotEvents_->header()->setStretchLastSection(true);
        pilotEvents_->setWordWrap(true);
        rv->addWidget(pilotEvents_, 1);
        split->addWidget(right);
        split->setStretchFactor(0, 5);
        split->setStretchFactor(1, 6);
        histStack_->addWidget(split);
    }
    histTable_ = new QTreeWidget;
    histTable_->setObjectName(QStringLiteral("historyTimeline"));
    histTable_->setRootIsDecorated(false);
    histTable_->setColumnCount(5);
    histTable_->header()->setStretchLastSection(true);
    histTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    histTable_->setWordWrap(true);
    histStack_->addWidget(histTable_);
    v->addWidget(histStack_, 1);
    auto *r = new QHBoxLayout;
    histHint_ = new QLabel;
    histHint_->setWordWrap(true);
    r->addWidget(histHint_, 1);
    histCopy_ = new QPushButton;
    connect(histCopy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(historyText(false)); });
    r->addWidget(histCopy_);
    histSave_ = new QPushButton;
    connect(histSave_, &QPushButton::clicked, this, &RaceHistory::save);
    r->addWidget(histSave_);
    histClear_ = new QPushButton;
    connect(histClear_, &QPushButton::clicked, this, &RaceHistory::clearRequested);
    r->addWidget(histClear_);
    v->addLayout(r);
    retranslate();
}

void RaceHistory::changeEvent(QEvent *e) {
    if (e->type() == QEvent::LanguageChange) retranslate();
    QWidget::changeEvent(e);
}

void RaceHistory::retranslate() {
    histTable_->setHeaderLabels({tr("Time"), tr("Race"), tr("Channel"), tr("Event"), tr("Details")});
    histTable_->headerItem()->setToolTip(1, tr("Time since race tracking started (sweep number in the tooltip)."));
    histHint_->setText(tr("Warnings, recommendations and changes of every race since the program started, and a "
                          "summary of each channel when it stops."));
    histRaceLbl_->setText(tr("Race:"));
    histView_->setItemText(0, tr("By pilot"));
    histView_->setItemText(1, tr("Timeline"));
    histView_->setToolTip(tr("By pilot: who had the most trouble and which, grouped as serious, warnings, channel "
                             "changes and information. Timeline: every event in order."));
    pilotTable_->setHeaderLabels({tr("Pilot"), tr("Video"), tr("Serious"), tr("Warnings"), tr("Changes"), tr("Info"),
                                  QString()});
    pilotTable_->headerItem()->setToolTip(0, tr("One pilot per pinned channel when the race started, with the "
                                                "channels it went through."));
    pilotTable_->headerItem()->setToolTip(1, tr("Estimated video quality (dB, more is better) and its SNR in the "
                                                "worst moments. After a move, only on its new channel."));
    pilotTable_->headerItem()->setToolTip(2, tr("Its channel gave it trouble: someone else on top of it or "
                                                "interference (move recommended, or no better free channel)."));
    pilotTable_->headerItem()->setToolTip(3, tr("Weak video that is not the channel's fault (antennas, power, "
                                                "distance), or a signal that would not go away."));
    pilotTable_->headerItem()->setToolTip(4, tr("Channel changes applied."));
    pilotEvents_->setHeaderLabels({tr("Event"), tr("Race"), tr("Details")});
    histCopy_->setText(tr("Copy"));
    histSave_->setText(tr("Save..."));
    histClear_->setText(tr("Clear"));
    rebuild();
}

QString RaceHistory::catName(Cat c) const {
    switch (c) {
    case Cat::Serious: return tr("Serious");
    case Cat::Warning: return tr("Warnings");
    case Cat::Change: return tr("Channel changes");
    case Cat::Info: break;
    }
    return tr("Information");
}

void RaceHistory::beginRace(const std::vector<race::ChannelHealth> &pilots, const QStringList &channels) {
    raceClock_.start();  // sus tiempos, desde aqui
    RaceRec r;
    r.no = races_.isEmpty() ? 1 : races_.back().no + 1;
    r.start = QDateTime::currentDateTime();
    for (size_t i = 0; i < pilots.size(); i++) {
        PilotRec p;
        p.channels << channels.value(static_cast<int>(i));
        p.health = pilots[i];
        r.pilots.push_back(p);
    }
    races_.push_back(r);
    pilotPicked_ = false;
    rebuild();
    // la nueva, elegida (si se estaba viendo una concreta)
    if (histRace_->currentIndex() > 0) {
        histRace_->setCurrentIndex(1);
        rebuild();
    }
}

void RaceHistory::syncHealth(const std::vector<race::ChannelHealth> &hs) {
    if (races_.isEmpty()) return;
    auto &ps = races_.back().pilots;
    for (int i = 0; i < static_cast<int>(hs.size()) && i < ps.size(); i++) ps[i].health = hs[i];
}

void RaceHistory::pilotMoved(int pilot, const QString &channel) {
    if (!races_.isEmpty() && pilot >= 0 && pilot < races_.back().pilots.size())
        races_.back().pilots[pilot].channels << channel;
}

void RaceHistory::clear(bool keepCurrent) {
    events_.clear();
    // la carrera en marcha sigue (sus eventos nuevos van a la misma); las ya terminadas se olvidan
    if (keepCurrent && !races_.isEmpty()) races_ = {races_.back()};
    else races_.clear();
    rebuild();
}

const RaceHistory::RaceRec *RaceHistory::raceRec(int no) const {
    for (const auto &r : races_)
        if (r.no == no) return &r;
    return nullptr;
}

QString RaceHistory::pilotLabel(int race, int pilot, bool withRace) const {
    const QString pre = withRace ? tr("Race %1").arg(race) + QStringLiteral(" · ") : QString();
    const RaceRec *r = raceRec(race);
    if (pilot < 0) return pre + tr("Whole race");
    QString chs;
    if (r && pilot < r->pilots.size()) chs = r->pilots[pilot].channels.join(QStringLiteral(" → "));
    return pre + tr("Pilot %1").arg(pilot + 1) + (chs.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(chs));
}

int RaceHistory::histRace() const { return histRace_->currentData().toInt(); }

bool RaceHistory::histShows(const RaceEvent &e) const {
    const int r = histRace();
    return r == 0 || e.race == r;
}

void RaceHistory::addEvent(Ev kind, int sweep, const QString &channel, const QString &what, const QString &detail,
                           int pilot) {
    RaceEvent e;
    e.when = QDateTime::currentDateTime();
    e.raceS = raceClock_.isValid() ? raceClock_.elapsed() / 1000.0 : 0;
    e.sweep = sweep;
    e.kind = kind;
    e.channel = channel;
    e.what = what;
    e.detail = detail;
    e.race = races_.isEmpty() ? 0 : races_.back().no;
    e.pilot = pilot;
    events_.push_back(e);
    if (histShows(e)) {
        addHistRow(e);
        histTable_->scrollToItem(histTable_->topLevelItem(histTable_->topLevelItemCount() - 1));
    }
    rebuildPilotView();
    countChanged();
}

void RaceHistory::countChanged() {
    histCopy_->setEnabled(!events_.isEmpty());
    histSave_->setEnabled(!events_.isEmpty());
    histClear_->setEnabled(!events_.isEmpty());
    emit eventsChanged(events_.size());
}

void RaceHistory::addHistRow(const RaceEvent &e) {
    const Ev kind = e.kind;
    auto *it = new QTreeWidgetItem(histTable_);
    it->setText(0, e.when.toString(QStringLiteral("HH:mm:ss")));
    it->setText(1, QStringLiteral("+") + raceTime(e.raceS));
    it->setToolTip(1, tr("Sweep %1").arg(e.sweep));
    it->setText(2, e.channel);
    it->setText(3, e.what);
    it->setText(4, e.detail);
    it->setToolTip(4, e.detail);
    it->setToolTip(2, pilotLabel(e.race, e.pilot, true));
    const QColor red(0xd9, 0x48, 0x3b), orange(0xe0, 0xa0, 0x20), green(0x3a, 0xa8, 0x5a), blue(0x2f, 0x80, 0xed);
    QColor col;
    switch (kind) {
    case Ev::Ok: col = green; break;
    case Ev::Warn: col = orange; break;
    case Ev::Problem:
    case Ev::Move: col = red; break;
    case Ev::Applied: col = blue; break;
    case Ev::Info:
    case Ev::Start:
    case Ev::Summary: break;
    }
    if (col.isValid()) it->setForeground(3, col);
    if (kind == Ev::Info || kind == Ev::Start || kind == Ev::Move) {
        QFont bold = histTable_->font();
        bold.setBold(true);
        it->setFont(3, bold);
    }
    // donde empieza cada carrera (tambien la que sigue a cambiar los fijados a mano)
    if (kind == Ev::Start)
        for (int c = 0; c < histTable_->columnCount(); c++) it->setBackground(c, QColor(0x80, 0x80, 0x80, 40));
    for (int c = 0; c < 4; c++) histTable_->resizeColumnToContents(c);
}

void RaceHistory::rebuild() {
    // carreras para elegir: todas y cada una (la ultima primero)
    const int want = histRace();
    {
        const QSignalBlocker b(histRace_);
        histRace_->clear();
        histRace_->addItem(tr("All"), 0);
        for (int i = races_.size() - 1; i >= 0; i--) {
            const RaceRec &r = races_[i];
            histRace_->addItem(tr("Race %1 (%2, %n pilot(s))", nullptr, int(r.pilots.size()))
                                   .arg(r.no).arg(r.start.toString(QStringLiteral("HH:mm"))),
                               r.no);
        }
        const int at = histRace_->findData(want);
        histRace_->setCurrentIndex(at >= 0 ? at : 0);
    }
    histTable_->clear();
    for (const RaceEvent &e : events_)
        if (histShows(e)) addHistRow(e);
    rebuildPilotView();
    countChanged();
}

QVector<RaceHistory::PilotRow> RaceHistory::pilotRows() const {
    // cuenta por piloto (carrera, indice) de cada categoria
    QVector<PilotRow> rows;
    const auto rowOf = [&](int race, int pilot) -> PilotRow & {
        for (PilotRow &r : rows)
            if (r.race == race && r.pilot == pilot) return r;
        PilotRow r;
        r.race = race;
        r.pilot = pilot;
        const RaceRec *rr = raceRec(race);
        if (rr && pilot >= 0 && pilot < rr->pilots.size()) r.rec = &rr->pilots[pilot];
        rows.push_back(r);
        return rows.back();
    };
    const int sel = histRace();
    for (const RaceRec &r : races_)
        if (sel == 0 || r.no == sel)
            for (int i = 0; i < r.pilots.size(); i++) rowOf(r.no, i);
    for (const RaceEvent &e : events_)
        if (e.race > 0 && histShows(e)) rowOf(e.race, e.pilot).n[static_cast<int>(category(e.kind))]++;
    // de mas a menos problemas: graves, avisos, peor video; lo de toda la carrera al final
    std::stable_sort(rows.begin(), rows.end(), [](const PilotRow &a, const PilotRow &b) {
        if ((a.pilot < 0) != (b.pilot < 0)) return b.pilot < 0;
        if (a.score() != b.score()) return a.score() > b.score();
        const float qa = a.rec && a.rec->health.seen > 0 ? a.rec->health.quality : 1e9f;
        const float qb = b.rec && b.rec->health.seen > 0 ? b.rec->health.quality : 1e9f;
        if (qa != qb) return qa < qb;
        return a.race != b.race ? a.race > b.race : a.pilot < b.pilot;
    });
    return rows;
}

QTreeWidgetItem *RaceHistory::addPilotRow(const PilotRow &r, bool withRace, const QFont &bold) {
    auto *it = new QTreeWidgetItem(pilotTable_);
    it->setData(0, Qt::UserRole, r.race);
    it->setData(0, Qt::UserRole + 1, r.pilot);
    it->setText(0, pilotLabel(r.race, r.pilot, withRace));
    if (r.rec && !r.rec->channels.isEmpty() && r.rec->health.ch.mhz > 0)
        it->setIcon(0, pinIcon(r.rec->health.ch.hdzero ? kHdzColour : kAnalogColour));
    if (r.rec) {
        const race::ChannelHealth &h = r.rec->health;
        if (h.seen > 0) {
            it->setText(1, QStringLiteral("%1 dB (SNR %2)").arg(h.quality, 0, 'f', 0).arg(h.snr_low, 0, 'f', 0));
            if (h.snr_low < kLowSnrDb) it->setForeground(1, catColour(1));
        } else {
            it->setText(1, h.sweeps > 0 ? tr("not seen") : QStringLiteral("-"));
        }
        it->setToolTip(1, healthText(h, true));
    }
    for (int c = 0; c < 4; c++) {
        it->setText(2 + c, QString::number(r.n[c]));
        it->setTextAlignment(2 + c, Qt::AlignCenter);
        if (r.n[c] > 0 && c < 3) {
            it->setForeground(2 + c, catColour(c));
            it->setFont(2 + c, bold);
        }
    }
    return it;
}

QString RaceHistory::verdictText(const QVector<PilotRow> &rows, bool withRace) const {
    const PilotRow *worst = nullptr;  // el primero: rows va de mas a menos problemas
    int pilots = 0;
    QStringList clean;  // los que no han tenido ninguno
    for (const PilotRow &r : rows) {
        if (r.pilot < 0) continue;
        pilots++;
        if (!worst) worst = &r;
        if (r.score() == 0) clean << pilotLabel(r.race, r.pilot, withRace).toHtmlEscaped();
    }
    if (!worst || worst->score() == 0) return pilots > 0 ? tr("No pilot had any trouble.") : QString();
    const QString who = pilotLabel(worst->race, worst->pilot, withRace).toHtmlEscaped();
    QString v = worst->neverSeen() ? tr("Most trouble: <b>%1</b>: its pilot was never seen.").arg(who)
                                   : tr("Most trouble: <b>%1</b>: %2 serious, %3 warning(s).")
                                         .arg(who).arg(worst->n[0]).arg(worst->n[1]);
    if (!clean.isEmpty()) v += QLatin1Char(' ') + tr("No trouble: %1.").arg(clean.join(QStringLiteral(", ")));
    return v;
}

void RaceHistory::rebuildPilotView() {
    const QVector<PilotRow> rows = pilotRows();
    // lo elegido a mano se conserva al rehacer; si no, el que mas problemas ha tenido (el primero)
    int keepRace = -1, keepPilot = -2;
    if (auto *cur = pilotTable_->currentItem(); cur && pilotPicked_) {
        keepRace = cur->data(0, Qt::UserRole).toInt();
        keepPilot = cur->data(0, Qt::UserRole + 1).toInt();
    }
    const QSignalBlocker b(pilotTable_);
    const int tableScroll = pilotTable_->verticalScrollBar()->value();  // rehecha en cada barrido: no salta arriba
    pilotTable_->clear();
    QFont bold = pilotTable_->font();
    bold.setBold(true);
    QTreeWidgetItem *keep = nullptr;
    const bool withRace = histRace() == 0 && races_.size() > 1;
    for (const PilotRow &r : rows) {
        QTreeWidgetItem *it = addPilotRow(r, withRace, bold);
        if (r.race == keepRace && r.pilot == keepPilot) keep = it;
    }
    // el que mas problemas ha tenido, resaltado (si alguno ha tenido)
    const auto worst = std::find_if(rows.begin(), rows.end(), [](const PilotRow &r) { return r.pilot >= 0; });
    if (worst != rows.end() && worst->score() > 0) {
        QTreeWidgetItem *w = pilotTable_->topLevelItem(0);
        for (int c = 0; c < pilotTable_->columnCount(); c++) w->setBackground(c, QColor(0xd9, 0x48, 0x3b, 50));
        w->setFont(0, bold);
        w->setToolTip(0, tr("The most trouble"));
    }
    histVerdict_->setText(verdictText(rows, withRace));
    for (int c = 0; c < pilotTable_->columnCount(); c++) pilotTable_->resizeColumnToContents(c);
    if (!keep) keep = pilotTable_->topLevelItem(0);
    if (keep) pilotTable_->setCurrentItem(keep);
    pilotTable_->verticalScrollBar()->setValue(tableScroll);
    showPilotEvents();
}

void RaceHistory::showPilotEvents() {
    // el mismo piloto que ya se mostraba (rehecho en cada barrido o evento): sus grupos abiertos o cerrados a mano y
    // el desplazamiento se conservan
    QTreeWidgetItem *now = pilotTable_->currentItem();
    const int nowRace = now ? now->data(0, Qt::UserRole).toInt() : -1;
    const int nowPilot = now ? now->data(0, Qt::UserRole + 1).toInt() : -2;
    const bool same = now && nowRace == shownRace_ && nowPilot == shownPilot_ &&
                      pilotEvents_->topLevelItemCount() == kGroups;
    const GroupsShown was = same ? groupsShown() : GroupsShown{};
    const int eventsScroll = pilotEvents_->verticalScrollBar()->value();
    shownRace_ = nowRace;
    shownPilot_ = nowPilot;
    pilotEvents_->clear();
    pilotInfo_->clear();
    if (!now) return;
    showPilotInfo(nowRace, nowPilot);
    const std::array<int, kGroups> count = addPilotEvents(nowRace, nowPilot);
    setGroupStates(count, same ? &was : nullptr);
    sizeEventColumns();
    if (same) pilotEvents_->verticalScrollBar()->setValue(eventsScroll);
}

RaceHistory::GroupsShown RaceHistory::groupsShown() const {
    GroupsShown g;
    for (int c = 0; c < kGroups; c++) {
        g.open[c] = pilotEvents_->topLevelItem(c)->isExpanded();
        g.empty[c] = pilotEvents_->topLevelItem(c)->childCount() == 0;
    }
    return g;
}

void RaceHistory::showPilotInfo(int race, int pilot) {
    const RaceRec *rr = raceRec(race);
    QString info = QStringLiteral("<b>%1</b>").arg(pilotLabel(race, pilot, true).toHtmlEscaped());
    if (rr && pilot >= 0 && pilot < rr->pilots.size())
        info += QStringLiteral(": ") + healthText(rr->pilots[pilot].health, true).toHtmlEscaped();
    pilotInfo_->setText(info);
}

// un grupo por categoria (aun sin texto) y en cada uno los eventos del piloto; devuelve cuantos hay en cada grupo
std::array<int, RaceHistory::kGroups> RaceHistory::addPilotEvents(int race, int pilot) {
    QFont bold = pilotEvents_->font();
    bold.setBold(true);
    QTreeWidgetItem *cats[kGroups] = {};
    for (int c = 0; c < kGroups; c++) {
        cats[c] = new QTreeWidgetItem(pilotEvents_);
        cats[c]->setFont(0, bold);
        cats[c]->setForeground(0, catColour(c));
        cats[c]->setFirstColumnSpanned(true);
    }
    std::array<int, kGroups> count{};
    for (const RaceEvent &e : events_) {
        if (e.race != race || e.pilot != pilot) continue;
        const int c = static_cast<int>(category(e.kind));
        count[c]++;
        auto *it = new QTreeWidgetItem(cats[c]);
        it->setText(0, e.what);
        it->setText(1, QStringLiteral("+") + raceTime(e.raceS));
        it->setToolTip(1, e.when.toString(QStringLiteral("HH:mm:ss")) + QStringLiteral(" · ") +
                              tr("Sweep %1").arg(e.sweep));
        it->setText(2, e.detail);
        it->setToolTip(2, e.detail);
        if (e.kind == Ev::Ok) it->setForeground(0, QColor(0x3a, 0xa8, 0x5a));
    }
    return count;
}

// el texto de cada grupo con su cuenta, los vacios atenuados, y cuales abiertos: los problemas abiertos y la
// informacion (OK, resumenes) cerrada, salvo que el usuario los haya dejado de otra forma (was: el mismo piloto)
void RaceHistory::setGroupStates(const std::array<int, kGroups> &count, const GroupsShown *was) {
    for (int c = 0; c < kGroups; c++) {
        QTreeWidgetItem *cat = pilotEvents_->topLevelItem(c);
        cat->setText(0, QStringLiteral("%1 (%2)").arg(catName(Cat(c))).arg(count[c]));
        if (count[c] == 0) {
            QColor dim = pilotEvents_->palette().color(QPalette::Text);  // Disabled no siempre se distingue (oscuro)
            dim.setAlpha(110);
            cat->setForeground(0, dim);
            cat->setFont(0, pilotEvents_->font());
            cat->setFlags(cat->flags() & ~Qt::ItemIsSelectable);
        }
        // un grupo que estaba vacio y ahora tiene algo, como si fuera nuevo
        const bool byDefault = count[c] > 0 && c != static_cast<int>(Cat::Info);
        cat->setExpanded(was && !was->empty[c] ? was->open[c] : byDefault);
    }
}

// con los grupos cerrados no se mide lo de dentro: un minimo para que no salga estrecha
void RaceHistory::sizeEventColumns() {
    const QFontMetrics fm(pilotEvents_->font());
    pilotEvents_->resizeColumnToContents(0);
    pilotEvents_->setColumnWidth(0, std::max(pilotEvents_->columnWidth(0), fm.horizontalAdvance(QLatin1Char('M')) * 18));
    pilotEvents_->resizeColumnToContents(1);
    pilotEvents_->setColumnWidth(1, std::max(pilotEvents_->columnWidth(1),
                                             fm.horizontalAdvance(QStringLiteral("+00:00  "))));
}

QString RaceHistory::healthText(const race::ChannelHealth &h, bool summary) {
    if (h.seen == 0) return summary ? tr("Its pilot was never seen.") : tr("Its pilot has not been seen.");
    const int intr = h.sweeps > 0 ? qRound(100.0 * h.intruders / h.sweeps) : 0;
    if (summary)
        return tr("SNR %1 / %2 dB, quality %3 dB, %4 dropout(s), intruders %5 %, interference %6 dB (%7 sweeps).")
            .arg(h.snr_med, 0, 'f', 0).arg(h.snr_low, 0, 'f', 0).arg(h.quality, 0, 'f', 0).arg(h.lost).arg(intr)
            .arg(h.channel_cost, 0, 'f', 1).arg(h.sweeps);
    return tr("SNR %1 / %2 dB, quality %3 dB, %4 dropout(s), intruders %5 %, interference %6 dB.")
        .arg(h.snr_med, 0, 'f', 0).arg(h.snr_low, 0, 'f', 0).arg(h.quality, 0, 'f', 0).arg(h.lost).arg(intr)
        .arg(h.channel_cost, 0, 'f', 1);
}

QString RaceHistory::historyText(bool csv) const {
    const auto q = [](QString v) {  // CSV: entre comillas, las comillas dobladas
        return QLatin1Char('"') + v.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
    };
    QStringList out;
    if (csv)
        out << QStringList{tr("Date"), tr("Time"), tr("Race"), tr("Sweep"), tr("Race no."), tr("Pilot"),
                           tr("Category"), tr("Channel"), tr("Event"), tr("Details")}
                   .join(QLatin1Char(','));
    for (const RaceEvent &e : events_) {
        const QString race = QStringLiteral("+") + raceTime(e.raceS);
        const QString cat = catName(category(e.kind));
        if (csv)
            out << QStringList{e.when.toString(QStringLiteral("yyyy-MM-dd")),
                               e.when.toString(QStringLiteral("HH:mm:ss")), race, QString::number(e.sweep),
                               QString::number(e.race), e.pilot >= 0 ? QString::number(e.pilot + 1) : QString(),
                               q(cat), q(e.channel), q(e.what), q(e.detail)}
                       .join(QLatin1Char(','));
        else
            out << QStringLiteral("%1  %2  [%3] %4%5%6")
                       .arg(e.when.toString(QStringLiteral("HH:mm:ss")), race.leftJustified(7), cat,
                            e.channel.isEmpty() ? QString() : e.channel + QStringLiteral(": "), e.what,
                            e.detail.isEmpty() ? QString() : QStringLiteral(" - ") + e.detail);
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void RaceHistory::save() {
    const QString def = QDir::home().filePath(
        QStringLiteral("carrera-%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))));
    const QString fn = QFileDialog::getSaveFileName(this, tr("Save race history"), def,
                                                    tr("CSV (*.csv);;Text (*.txt)"));
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        KP_LOGW("carrera: no se puede guardar el historial en %s", qUtf8Printable(fn));
        histHint_->setText(tr("Could not save %1.").arg(QDir::toNativeSeparators(fn)));
        return;
    }
    f.write(historyText(!fn.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)).toUtf8());
    KP_LOGI("carrera: historial guardado en %s", qUtf8Printable(fn));
    histHint_->setText(tr("Saved to %1.").arg(QDir::toNativeSeparators(fn)));
}
