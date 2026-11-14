#include "main_window.h"
#include "stub_backend.h"
#ifdef KP_WITH_RADIO
#include "radio_backend.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
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
    QApplication::setApplicationName(QStringLiteral("kepyur-desktop"));
    QApplication::setApplicationVersion(QStringLiteral(KP_VERSION));
    qRegisterMetaType<Pilot>();
    qRegisterMetaType<QVector<Pilot>>();
    qRegisterMetaType<RadioStatus>();

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Receptor FPV analogico 5.8 GHz con un adaptador RTL8812BU (kepyur)"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption demo(QStringLiteral("demo"), QStringLiteral("Sin hardware: video y pilotos simulados"));
    QCommandLineOption autoOpt(QStringLiteral("auto"), QStringLiteral("Conectar y escanear al arrancar"));
    QCommandLineOption verbose(QStringLiteral("verbose"), QStringLiteral("Log de depuracion"));
    QCommandLineOption watchOpt(QStringLiteral("watch"), QStringLiteral("Conectar y ver esta frecuencia al arrancar"),
                                QStringLiteral("MHz"));
    QCommandLineOption rateOpt(QStringLiteral("rate"), QStringLiteral("Muestreo para ver: 40 o 20 (MS/s)"),
                               QStringLiteral("MSps"));
    cli.addOptions({demo, autoOpt, verbose, watchOpt, rateOpt});
    cli.process(app);

    // Log de kepyur/wifi-sdr: stderr + fichero (como kepyur.log en Android)
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(logDir);
    const QString logPath = logDir + "/kepyur.log";
    g_log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    kp::set_log_sink(file_sink);
    kp::set_log_debug(cli.isSet(verbose));
    KP_LOGI("kepyur-desktop %s start, log %s", KP_VERSION, qPrintable(logPath));

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
    w.resize(1100, 640);
    w.show();
    const double watchMHz = cli.value(watchOpt).toDouble();
    if (cli.isSet(autoOpt) || cli.isSet(demo) || watchMHz > 0) {
        QTimer::singleShot(0, &w, [&w] { emit w.requestOpen(); });
        QObject::connect(backend, &Backend::opened, &w, [&w, watchMHz](bool ok) {
            if (!ok) return;
            if (watchMHz > 0) emit w.requestWatch(watchMHz);
            else emit w.requestScan();
        }, Qt::SingleShotConnection);
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
