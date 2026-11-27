#pragma once
// Manejar las ventanas de los analizadores (src/channel_analyzer.h, src/elrs_analyzer.h) como el usuario: por sus
// controles (objectName), sin entrar en su estado. Para las pruebas y las capturas de test/.
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QImage>
#include <QMouseEvent>
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

// El raton sobre una grafica en x (a media altura), sin pulsar
inline void moveMouse(QWidget *view, int x) {
    const QPointF at(x, view->height() / 2);
    QMouseEvent move(QEvent::MouseMove, at, view->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view, &move);
}

// Pasar el raton por una grafica en x (a media altura) y sacarlo: lo pintado antes, con el raton encima y despues
struct Hover {
    QImage before, over, after;
    // con el raton encima ha cambiado algo en la columna x
    bool marksColumn(int x) const {
        for (int y = 0; y < before.height(); y++)
            if (before.pixel(x, y) != over.pixel(x, y)) return true;
        return false;
    }
};
inline Hover hover(QWidget *view, int x) {
    Hover h;
    view->grab();  // la primera captura aun pule el widget (fuente, estilo): no vale para comparar
    h.before = view->grab().toImage();
    moveMouse(view, x);
    h.over = view->grab().toImage();
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(view, &leave);
    h.after = view->grab().toImage();
    return h;
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

// Los eventos del piloto elegido en el Historial: primero lo que dice de el (sin HTML), luego cada grupo con su
// cuenta y si esta abierto ("Serious (2) [+]", "Info (1) [-]") y debajo sus eventos ("  que | +00:05 | detalle")
inline QStringList pilotEvents(const ChannelAnalyzer &w) {
    QStringList out{find<QLabel>(&w, "historyPilotInfo")->text().remove(QRegularExpression(QStringLiteral("<[^>]*>")))};
    const auto *t = find<QTreeWidget>(&w, "historyEvents");
    for (int i = 0; i < t->topLevelItemCount(); i++) {
        const QTreeWidgetItem *g = t->topLevelItem(i);
        out << g->text(0) + (g->isExpanded() ? QStringLiteral(" [+]") : QStringLiteral(" [-]"));
        for (int k = 0; k < g->childCount(); k++) {
            const QTreeWidgetItem *e = g->child(k);
            out << QStringLiteral("  %1 | %2 | %3").arg(e->text(0), e->text(1), e->text(2));
        }
    }
    return out;
}
// Abre o cierra a mano un grupo de eventos del piloto elegido (0 graves, 1 avisos, 2 cambios, 3 informacion)
inline void setEventGroupOpen(ChannelAnalyzer &w, int group, bool open) {
    find<QTreeWidget>(&w, "historyEvents")->topLevelItem(group)->setExpanded(open);
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
// las filas de "En el aire": "senal | frecuencia | pico | actividad", con " (tenue)" si lleva un rato sin verse
template <class W>
QStringList detected(const W &w) {
    const auto *t = find<QTreeWidget>(&w, "detected");
    QStringList out;
    for (int r = 0; r < t->topLevelItemCount(); r++) {
        const QTreeWidgetItem *it = t->topLevelItem(r);
        QStringList cols;
        for (int c = 0; c < t->columnCount(); c++) cols << it->text(c);
        const bool dim = it->foreground(1).color().alpha() < 255;
        out << cols.join(QStringLiteral(" | ")) + (dim ? QStringLiteral(" (tenue)") : QString());
    }
    return out;
}

}  // namespace drive
