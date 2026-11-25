// Captura del analizador de 2,4 GHz (ELRS) con el backend de demostracion (sin adaptador), para revisar la ventana:
//   elrs_shot SALIDA.png [SEGUNDOS] [--qm TRADUCCION.qm] [--use 0|1|2] [--eu] [--pilots N]
// (QT_QPA_PLATFORM=offscreen: sin pantalla). Usa su propio .ini (en la carpeta temporal), no el del usuario.
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

#include "elrs_analyzer.h"
#include "stub_backend.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStringList pos;
    QTranslator tr;
    int use = 1;
    bool eu = false;
    int pilots = 0;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--qm") && i + 1 < argc) {
            if (tr.load(QString::fromLocal8Bit(argv[++i]))) QCoreApplication::installTranslator(&tr);
        } else if (a == QLatin1String("--use") && i + 1 < argc) use = QString::fromLocal8Bit(argv[++i]).toInt();
        else if (a == QLatin1String("--eu")) eu = true;
        else if (a == QLatin1String("--pilots") && i + 1 < argc) pilots = QString::fromLocal8Bit(argv[++i]).toInt();
        else pos << a;
    }
    if (pos.isEmpty()) return 2;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::tempPath() + QStringLiteral("/elrs_shot"));
    {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("FPVDesktopWiFiReceiver"),
                    QStringLiteral("FPVDesktopWiFiReceiver"));
        s.setValue("elrs/use", use);
        s.setValue("elrs/region", eu ? 1 : 0);
        s.setValue("elrs/pilots", pilots);
    }
    qRegisterMetaType<SurveyData>();
    qRegisterMetaType<RadioStatus>();
    StubBackend backend;
    QThread th;
    backend.moveToThread(&th);
    th.start();
    ElrsAnalyzer w(&backend);
    w.setAdapterOpen(true);
    w.show();
    const QString out = pos[0];
    const int secs = pos.size() > 1 ? pos[1].toInt() : 5;
    QTimer::singleShot(200, &w, [&] { w.startForTest(); });
    QTimer::singleShot(secs * 1000, &w, [&] {
        w.grab().save(out);
        std::printf("%s\n", qPrintable(w.tipsTextForTest()));
        app.quit();
    });
    const int rc = app.exec();
    th.quit();
    th.wait();
    return rc;
}
