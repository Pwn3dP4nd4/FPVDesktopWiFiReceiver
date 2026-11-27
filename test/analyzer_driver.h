#pragma once
// Manejar las ventanas de los analizadores (src/channel_analyzer.h, src/elrs_analyzer.h) como el usuario: por sus
// controles (objectName), sin entrar en su estado. Para las pruebas y las capturas de test/.
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdio>
#include <cstdlib>

#include "channel_analyzer.h"

namespace drive {

template <class T>
T *find(const QWidget *w, const char *name) {
    T *c = w->findChild<T *>(QString::fromLatin1(name));
    if (!c) {
        std::fprintf(stderr, "analyzer_driver: no hay %s\n", name);
        std::abort();
    }
    return c;
}

inline void setChecked(QAbstractButton *b, bool on) {
    if (b->isChecked() != on) b->setChecked(on);
}

// Empezar el analisis (boton Empezar), si no esta en marcha
template <class W>
void start(W &w) {
    if (!w.isRunning()) find<QPushButton>(&w, "start")->click();
}

// ---- analizador de 5,8 GHz ----

inline void setCounts(ChannelAnalyzer &w, int analog, int hdzero) {
    if (analog >= 0) find<QSpinBox>(&w, "analogCount")->setValue(analog);
    if (hdzero >= 0) find<QSpinBox>(&w, "hdzeroCount")->setValue(hdzero);
}

inline SpectrumView *view(const ChannelAnalyzer &w) { return w.findChild<SpectrumView *>(); }

// Los canales fijados, como en la grafica (clic derecho): quitar todos y fijar estos ("R3", "hdz:R3")
inline void setPinned(ChannelAnalyzer &w, const QStringList &pins) {
    SpectrumView *v = view(w);
    emit v->unpinAllRequested();
    for (const QString &p : pins) {
        const bool hdz = p.startsWith(QLatin1String("hdz:"));
        emit v->pinToggled(hdz ? p.mid(4) : p, hdz);
    }
}

// Abrir el desplegable de canales de una banda ('R', 'A'...); nullptr si la banda no esta marcada
inline QFrame *openChannelMenu(ChannelAnalyzer &w, char band) {
    const QByteArray name = QByteArray("channels:") + band;
    auto *btn = find<QToolButton>(&w, name.constData());
    if (!btn->isEnabled()) return nullptr;
    btn->click();
    for (QFrame *pop : w.findChildren<QFrame *>(QStringLiteral("channelMenu")))
        if (pop->isVisible()) return pop;
    return nullptr;
}

// Los canales que no se analizan ("R2"), con el desplegable de cada banda marcada; los demas, todos
inline void setExcluded(ChannelAnalyzer &w, const QStringList &names) {
    for (char band : QByteArrayLiteral("ABEFRL")) {
        QFrame *pop = openChannelMenu(w, band);
        if (!pop) continue;
        for (QCheckBox *cb : pop->findChildren<QCheckBox *>()) {
            const QString ch = cb->text().section(QLatin1Char(' '), 0, 0);
            setChecked(cb, !names.contains(ch));
        }
        delete pop;  // (cerrado tambien se borraria, pero mas tarde: el de la banda siguiente es otro)
    }
}

// Marcar como mios en "En el aire" (clave: el nombre del canal, o sus MHz); los demas, sin marcar
inline void setMine(ChannelAnalyzer &w, const QStringList &keys) {
    auto *air = find<QListWidget>(&w, "onTheAir");
    for (int i = 0; i < air->count(); i++) {
        QListWidgetItem *it = air->item(i);
        if (!(it->flags() & Qt::ItemIsUserCheckable)) continue;
        it->setCheckState(keys.contains(it->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
    }
}

inline void setRace(ChannelAnalyzer &w, bool on) {
    find<QTabWidget>(&w, "tabs")->setCurrentIndex(on ? 1 : 0);
    setChecked(find<QCheckBox>(&w, "race"), on);
}
inline bool raceOn(const ChannelAnalyzer &w) { return find<QCheckBox>(&w, "race")->isChecked(); }

// "Empezar al salir los pilotos" con este numero de pilotos (<= 0: todos los fijados)
inline void arm(ChannelAnalyzer &w, int pilots) {
    find<QTabWidget>(&w, "tabs")->setCurrentIndex(1);
    auto *n = find<QSpinBox>(&w, "armCount");
    n->setValue(pilots <= 0 ? n->maximum() : pilots);
    setChecked(find<QPushButton>(&w, "arm"), true);
}
inline bool armed(const ChannelAnalyzer &w) { return find<QPushButton>(&w, "arm")->isChecked(); }

// Barridos del seguimiento de carrera, como los dice el estado de la carrera; -1 si no lo dice
inline int raceSweeps(const ChannelAnalyzer &w) {
    const QString s = find<QLabel>(&w, "raceState")->text();
    for (const char *re : {"sweep (\\d+) of", "(\\d+) sweeps\\.", "after (\\d+) sweep"}) {
        const auto m = QRegularExpression(QLatin1String(re)).match(s);
        if (m.hasMatch()) return m.captured(1).toInt();
    }
    return -1;
}

// Aplicar las recomendaciones de la carrera (boton Aplicar)
inline void apply(ChannelAnalyzer &w) { find<QPushButton>(&w, "apply")->click(); }

// Pestana Historial, por piloto o cronologico
inline void showHistory(ChannelAnalyzer &w, bool timeline = false) {
    find<QTabWidget>(&w, "tabs")->setCurrentIndex(2);
    find<QComboBox>(&w, "historyView")->setCurrentIndex(timeline ? 1 : 0);
}

// Lo que dice cada evento del Historial (vista cronologica, todas las carreras)
inline QStringList history(const ChannelAnalyzer &w) {
    QStringList l;
    const auto *t = find<QTreeWidget>(&w, "historyTimeline");
    for (int i = 0; i < t->topLevelItemCount(); i++) l << t->topLevelItem(i)->text(3);
    return l;
}

// La tabla por piloto del Historial, de mas a menos problemas: "R3>R2: 2 serious, 1 warnings, 1 changes (race 1)";
// lo de toda la carrera, "race: ..."
inline QStringList pilotSummary(const ChannelAnalyzer &w) {
    QStringList out;
    const auto *t = find<QTreeWidget>(&w, "historyPilots");
    for (int i = 0; i < t->topLevelItemCount(); i++) {
        const QTreeWidgetItem *it = t->topLevelItem(i);
        const int race = it->data(0, Qt::UserRole).toInt(), pilot = it->data(0, Qt::UserRole + 1).toInt();
        // "Pilot 2 (R3 → R2)": los canales entre parentesis
        const auto m = QRegularExpression(QStringLiteral("\\(([^)]*)\\)$")).match(it->text(0));
        const QString who = pilot < 0 ? QStringLiteral("race")
                            : m.hasMatch() ? m.captured(1).replace(QStringLiteral(" → "), QStringLiteral(">"))
                                           : QString::number(pilot);
        out << QStringLiteral("%1: %2 serious, %3 warnings, %4 changes (race %5)")
                   .arg(who, it->text(2), it->text(3), it->text(4), QString::number(race));
    }
    return out;
}

// ---- analizador de 2,4 GHz ----

template <class W>
QString tips(const W &w) {
    return find<QTextBrowser>(&w, "tips")->toPlainText();
}
// el veredicto que se muestra (al principio del resumen: "Clean band", "Heavy interference"...)
template <class W>
bool shows(const W &w, const QString &verdict) {
    return find<QLabel>(&w, "summary")->text().startsWith(verdict);
}

}  // namespace drive
