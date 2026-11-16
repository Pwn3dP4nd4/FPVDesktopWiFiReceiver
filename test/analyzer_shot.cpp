// Captura del analizador de canales con el backend de demostracion (sin adaptador), para revisar la ventana:
//   analyzer_shot SALIDA.png [ANALOGICOS HDZERO [SEGUNDOS]]      (QT_QPA_PLATFORM=offscreen: sin pantalla)
#include <QApplication>
#include <QThread>
#include <QTimer>

#include "channel_analyzer.h"
#include "stub_backend.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) return 2;
    qRegisterMetaType<SurveyData>();
    qRegisterMetaType<RadioStatus>();
    StubBackend backend;
    QThread th;
    backend.moveToThread(&th);
    th.start();
    ChannelAnalyzer w(&backend);
    w.setAdapterOpen(true);
    w.show();
    const QString out = QString::fromLocal8Bit(argv[1]);
    const int na = argc > 3 ? atoi(argv[2]) : -1, nh = argc > 3 ? atoi(argv[3]) : -1;
    const int secs = argc > 4 ? atoi(argv[4]) : 4;
    QTimer::singleShot(200, &w, [&] {
        w.setCounts(na, nh);
        w.startForTest();
    });
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
