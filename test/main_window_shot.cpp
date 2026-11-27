// Captura de la ventana principal con el backend de demostracion (sin adaptador), para revisar la ventana: conecta,
// escanea y se queda viendo el piloto de demo, como al abrir la app con --demo.
//   main_window_shot SALIDA.png [SEGUNDOS]
// (QT_QPA_PLATFORM=offscreen: sin pantalla). Usa su propio .ini (en la carpeta temporal), no el del usuario.
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <cstdio>

#include "main_window.h"
#include "stub_backend.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "uso: main_window_shot SALIDA.png [SEGUNDOS]\n");
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    const int secs = argc > 2 ? std::atoi(argv[2]) : 6;
    // un .ini nuevo cada vez: la geometria y los ajustes de una captura anterior no cuentan
    const QString ini = QDir::tempPath() + QStringLiteral("/main_window_shot");
    QDir(ini).removeRecursively();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, ini);
    qRegisterMetaType<SurveyData>();
    qRegisterMetaType<RadioStatus>();
    qRegisterMetaType<Pilot>();
    qRegisterMetaType<QVector<Pilot>>();
    qRegisterMetaType<QVector<double>>();
    auto *backend = new StubBackend;
    QThread th;
    backend->moveToThread(&th);
    th.start();
    int rc = 0;
    {
        MainWindow w(backend);
        w.show();
        QTimer::singleShot(0, &w, [&w] { w.startup(true, 0, true); });
        QTimer::singleShot(secs * 1000, &w, [&] {
            if (!w.grab().save(out)) rc = 1;
            w.close();
            app.quit();
        });
        app.exec();
    }
    QMetaObject::invokeMethod(backend, [backend] {
        backend->closeDevice();
        delete backend;
    }, Qt::BlockingQueuedConnection);
    th.quit();
    th.wait();
    return rc;
}
