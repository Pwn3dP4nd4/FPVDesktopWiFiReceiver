// Captura del analizador de canales con el backend de demostracion (sin adaptador), para revisar la ventana:
//   analyzer_shot SALIDA.png [ANALOGICOS HDZERO [SEGUNDOS]] [--pin R1,R3,hdz:R6] [--race] [--mine R4/A6/B3]
//   [--qm TRADUCCION.qm] [--dark]
// (QT_QPA_PLATFORM=offscreen: sin pantalla). --pin: canales fijados (hdz: = HDZero); --race: escenario de carrera
// del backend de demo y el seguimiento de carrera activado; --mine: pilotos marcados como mios en "En el aire"
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

#include "channel_analyzer.h"
#include "stub_backend.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStringList pos, pins, mine;
    bool race = false;
    QTranslator tr;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--race")) race = true;
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
    w.setPinnedForTest(pins);  // tambien sin --pin: lo de una captura anterior no se queda
    w.show();
    const QString out = pos[0];
    const int na = pos.size() > 2 ? pos[1].toInt() : -1, nh = pos.size() > 2 ? pos[2].toInt() : -1;
    const int secs = pos.size() > 3 ? pos[3].toInt() : 4;
    QTimer::singleShot(200, &w, [&] {
        w.setCounts(na, nh);
        w.startForTest();
        if (race) w.setRaceForTest(true);
    });
    // cuando ya se ven los pilotos (empezar borra los marcados)
    if (!mine.isEmpty()) QTimer::singleShot(2000, &w, [&] { w.setMineForTest(mine); });
    QTimer::singleShot(secs * 1000, &w, [&] {
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
