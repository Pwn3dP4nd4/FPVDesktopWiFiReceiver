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
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif
#include <mutex>

#include "i18n.h"
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

    // Idioma guardado (o el del sistema) antes de crear ningun texto
    i18n::apply(i18n::setting());

    QCommandLineParser cli;
    cli.setApplicationDescription(QCoreApplication::translate(
        "main", "FPVDesktopWiFiReceiver: analog 5.8 GHz FPV receiver with an RTL8812BU adapter. "
                "Without options it connects the adapter on startup (configurable in Options)."));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption demo(QStringLiteral("demo"), QCoreApplication::translate("main", "No hardware: simulated video and pilots"));
    QCommandLineOption autoOpt(QStringLiteral("auto"),
                               QCoreApplication::translate("main", "Connect and scan on startup (even if disabled in Options)"));
    QCommandLineOption noAuto(QStringLiteral("no-connect"), QCoreApplication::translate("main", "Don't connect the adapter on startup"));
    QCommandLineOption verbose(QStringLiteral("verbose"), QCoreApplication::translate("main", "Debug log"));
    QCommandLineOption watchOpt(QStringLiteral("watch"), QCoreApplication::translate("main", "Connect and watch this frequency on startup"),
                                QStringLiteral("MHz"));
    QCommandLineOption rateOpt(QStringLiteral("rate"), QCoreApplication::translate("main", "Sample rate when watching: 40 or 20 (MS/s)"),
                               QStringLiteral("MSps"));
    QCommandLineOption langOpt(QStringLiteral("lang"), QCoreApplication::translate("main", "Interface language: en, es or system (saved)"),
                               QStringLiteral("lang"));
    cli.addOptions({demo, autoOpt, noAuto, verbose, watchOpt, rateOpt, langOpt});
    cli.process(app);
    if (cli.isSet(langOpt)) {
        const QString l = cli.value(langOpt).toLower();
        i18n::apply(l == QLatin1String("en") || l == QLatin1String("es") ? l : QString());
    }

    // Log de kepyur/wifi-sdr: stderr + fichero (como kepyur.log en Android)
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(logDir);
    const QString logPath = logDir + "/FPVDesktopWiFiReceiver.log";
    g_log = std::fopen(QFile::encodeName(logPath).constData(), "a");
    kp::set_log_sink(file_sink);
    kp::set_log_debug(cli.isSet(verbose));
    KP_LOGI("FPVDesktopWiFiReceiver %s start, log %s", KP_VERSION, qPrintable(logPath));
#ifdef _WIN32
    // 65001 = UTF-8 (manifiesto): las rutas con tildes/enes funcionan tambien en el fopen() de kepyur/wifi-sdr
    KP_LOGI("windows code page %u%s", GetACP(), GetACP() == CP_UTF8 ? "" : " (no UTF-8: rutas no ASCII pueden fallar)");
    {
        // devourer escribe ~45.000 registros al arrancar el chip con un sleep_for(1 us) tras cada uno. En
        // Windows cada sleep dura lo que diga el temporizador del proceso: con EcoQoS (ahorro de energia de
        // Windows 11) o la resolucion por defecto (15,6 ms) la apertura pasa de ~8 s a minutos. Se pide no
        // aplicar el ahorro a este proceso y temporizador de 1 ms mientras la app este abierta.
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;  // ninguno de los dos ahorros
        const bool ptOk = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
        const MMRESULT tr = timeBeginPeriod(1);
        KP_LOGI("windows: power throttling off %s, timer 1 ms %s", ptOk ? "ok" : "fallo",
                tr == TIMERR_NOERROR ? "ok" : "fallo");
    }
#endif

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
