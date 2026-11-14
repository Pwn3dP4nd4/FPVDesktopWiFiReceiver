#include "main_window.h"
#include "stub_backend.h"
#ifdef KP_WITH_RADIO
#include "radio_backend.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <mutex>

#include "log.h"

namespace {
std::mutex g_log_m;
FILE *g_log = nullptr;
void file_sink(kp::LogLevel l, const char *line) {
    static const char tag[] = {'D', 'I', 'W', 'E'};
    const QByteArray ts = QTime::currentTime().toString("HH:mm:ss.zzz").toLatin1();
    std::lock_guard<std::mutex> lk(g_log_m);
    if (!g_log) return;
    std::fprintf(g_log, "%s %c %s\n", ts.constData(), tag[static_cast<int>(l)], line);
    std::fflush(g_log);
}
}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("FPVDesktopWiFiReceiver"));
    QApplication::setApplicationVersion(QStringLiteral(KP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("FPVDesktopWiFiReceiver"));  // Wayland: icono del .desktop
    QIcon icon;
    for (int n : {16, 24, 32, 48, 64, 128, 256}) icon.addFile(QStringLiteral(":/icons/app-%1.png").arg(n), QSize(n, n));
    QApplication::setWindowIcon(icon);
    qRegisterMetaType<Pilot>();
    qRegisterMetaType<QVector<Pilot>>();
    qRegisterMetaType<RadioStatus>();

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("FPVDesktopWiFiReceiver: receptor FPV analogico 5.8 GHz con un adaptador RTL8812BU. "
                                                    "Sin opciones conecta el adaptador al abrir (ajustable en Opciones)."));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption demo(QStringLiteral("demo"), QStringLiteral("Sin hardware: video y pilotos simulados"));
    QCommandLineOption autoOpt(QStringLiteral("auto"), QStringLiteral("Conectar y escanear al arrancar (aunque este desactivado en Opciones)"));
    QCommandLineOption noAuto(QStringLiteral("no-connect"), QStringLiteral("No conectar el adaptador al abrir"));
    QCommandLineOption verbose(QStringLiteral("verbose"), QStringLiteral("Log de depuracion"));
    QCommandLineOption watchOpt(QStringLiteral("watch"), QStringLiteral("Conectar y ver esta frecuencia al arrancar"),
                                QStringLiteral("MHz"));
    QCommandLineOption rateOpt(QStringLiteral("rate"), QStringLiteral("Muestreo para ver: 40 o 20 (MS/s)"),
                               QStringLiteral("MSps"));
    cli.addOptions({demo, autoOpt, noAuto, verbose, watchOpt, rateOpt});
    cli.process(app);

    // Log de kepyur/wifi-sdr: stderr + fichero (como kepyur.log en Android)
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(logDir);
    const QString logPath = logDir + "/FPVDesktopWiFiReceiver.log";
    g_log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    kp::set_log_sink(file_sink);
    kp::set_log_debug(cli.isSet(verbose));
    KP_LOGI("FPVDesktopWiFiReceiver %s start, log %s", KP_VERSION, qPrintable(logPath));

    Backend *backend = nullptr;
#ifdef KP_WITH_RADIO
    if (!cli.isSet(demo)) backend = new RadioBackend;
#endif
    if (!backend) backend = new StubBackend;

    QThread thread;
    thread.setObjectName(QStringLiteral("backend"));
    backend->moveToThread(&thread);
    thread.start();

    MainWindow w(backend);
    if (cli.isSet(rateOpt)) w.setRate(cli.value(rateOpt) == QLatin1String("40") ? 1 : 2);
    w.show();
    // Doble clic en el .exe: conecta solo (ajuste "Conectar el adaptador al abrir", activo por defecto) y
    // vuelve al ultimo canal visto o escanea. La linea de comandos solo hace falta para forzar otra cosa.
    const double watchMHz = cli.value(watchOpt).toDouble();
    if (!cli.isSet(noAuto)) {
        const bool forceScan = cli.isSet(autoOpt) || cli.isSet(demo);
        QTimer::singleShot(0, &w, [&w, watchMHz, forceScan] { w.startup(forceScan, watchMHz, forceScan); });
    }

    const int rc = app.exec();

    // Cerrar la radio en su propio hilo (para los timers) y luego destruirla alli mismo.
    QMetaObject::invokeMethod(backend, [backend] {
        backend->closeDevice();
        delete backend;
    }, Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
    {
        std::lock_guard<std::mutex> lk(g_log_m);
        if (g_log) std::fclose(g_log);
        g_log = nullptr;
    }
    return rc;
}
