// Captura del analizador de canales con el backend de demostracion (sin adaptador), para revisar la ventana:
//   analyzer_shot SALIDA.png [ANALOGICOS HDZERO [SEGUNDOS]] [--pin R1,R3,hdz:R6] [--race] [--auto N] [--history] [--idle] [--exclude R2,R7] [--menu R] [--apply] [--mine R4/A6/B3]
//   [--qm TRADUCCION.qm] [--dark]
// (QT_QPA_PLATFORM=offscreen: sin pantalla). --pin: canales fijados (hdz: = HDZero); --race: escenario de carrera
// del backend de demo y el seguimiento de carrera activado; --apply: pulsa Aplicar 1.5 s antes de la captura; --mine: pilotos marcados como mios en "En el aire"
// (su nombre o sus MHz, a los 2 s); --dark: tema oscuro (Fusion); --qm: textos traducidos (build/x/*_es.qm). Usa su
// propio .ini (en la carpeta temporal), no el del usuario.
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QDir>
#include <QSettings>
#include <QThread>
#include <QTranslator>
#include <QTimer>

#include "analyzer_driver.h"
#include "stub_backend.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStringList pos, pins, mine, excl;
    int menuBand = -1;  // --menu R: abre el desplegable de canales de esa banda
    bool race = false, apply = false, history = false, idle = false;  // --idle: sin empezar el analisis
    int autoN = -1;  // --auto N: armar el auto-inicio (0 = todos los fijados) en vez de --race
    QTranslator tr;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--race")) race = true;
        else if (a == QLatin1String("--auto") && i + 1 < argc) {
            autoN = QString::fromLocal8Bit(argv[++i]).toInt();
            race = true;
        } else if (a == QLatin1String("--history")) history = true;
        else if (a == QLatin1String("--idle")) idle = true;
        else if (a == QLatin1String("--exclude") && i + 1 < argc)
            excl = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--menu") && i + 1 < argc)
            menuBand = QString::fromLatin1("ABEFRL").indexOf(QString::fromLocal8Bit(argv[++i]).left(1));
        else if (a == QLatin1String("--apply")) apply = true;
        else if (a == QLatin1String("--pin") && i + 1 < argc)
            pins = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--dark")) {
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
            QPalette d;
            const QColor win(45, 45, 48), base(30, 30, 32), text(230, 230, 230);
            d.setColor(QPalette::Window, win);
            d.setColor(QPalette::WindowText, text);
            d.setColor(QPalette::Base, base);
            d.setColor(QPalette::AlternateBase, win);
            d.setColor(QPalette::Text, text);
            d.setColor(QPalette::Button, win);
            d.setColor(QPalette::ButtonText, text);
            d.setColor(QPalette::Mid, QColor(20, 20, 20));
            d.setColor(QPalette::Highlight, QColor(42, 130, 218));
            QApplication::setPalette(d);
        } else if (a == QLatin1String("--mine") && i + 1 < argc)
            mine = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char(','), Qt::SkipEmptyParts);
        else if (a == QLatin1String("--qm") && i + 1 < argc) {
            if (tr.load(QString::fromLocal8Bit(argv[++i]))) QCoreApplication::installTranslator(&tr);
        } else pos << a;
    }
    if (pos.isEmpty()) return 2;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath() + QStringLiteral("/analyzer_shot"));
    qRegisterMetaType<SurveyData>();
    qRegisterMetaType<RadioStatus>();
    StubBackend backend;
    backend.setRaceDemo(race);
    QThread th;
    backend.moveToThread(&th);
    th.start();
    ChannelAnalyzer w(&backend);
    w.setAdapterOpen(true);
    drive::setPinned(w, pins);  // tambien sin --pin: lo de una captura anterior no se queda
    w.show();
    const QString out = pos[0];
    const int na = pos.size() > 2 ? pos[1].toInt() : -1, nh = pos.size() > 2 ? pos[2].toInt() : -1;
    const int secs = pos.size() > 3 ? pos[3].toInt() : 4;
    QTimer::singleShot(200, &w, [&] {
        drive::setCounts(w, na, nh);
        drive::setExcluded(w, excl);  // tambien sin --exclude: lo de una captura anterior no se queda
        if (menuBand >= 0) drive::openChannelMenu(w, "ABEFRL"[menuBand]);
        if (idle) return;
        if (autoN >= 0) drive::arm(w, autoN);  // arranca el analisis y espera a los pilotos
        else {
            drive::start(w);
            if (race) drive::setRace(w, true);
        }
    });
    // cuando ya se ven los pilotos (empezar borra los marcados)
    if (!mine.isEmpty()) QTimer::singleShot(2000, &w, [&] { drive::setMine(w, mine); });
    if (apply) QTimer::singleShot(secs * 1000 - 1500, &w, [&] { drive::apply(w); });
    QTimer::singleShot(secs * 1000, &w, [&] {
        if (history) drive::showHistory(w);
        // el desplegable de canales es otra ventana: su propia captura (SALIDA-menu.png)
        if (QWidget *pop = QApplication::activePopupWidget()) {
            QString m = out;
            pop->grab().save(m.replace(QStringLiteral(".png"), QStringLiteral("-menu.png")));
        }
        w.grab().save(out);
        w.close();
        app.quit();
    });
    const int rc = app.exec();
    QMetaObject::invokeMethod(&backend, [&] { backend.stopSurvey(); }, Qt::BlockingQueuedConnection);
    th.quit();
    th.wait();
    return rc;
}
