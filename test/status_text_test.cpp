// Textos del estado de la radio en la ventana principal (src/status_text.h): el OSD sobre el video y el recuadro de
// informacion del panel lateral (modo desarrollador, errores y avisos). Sin ventana ni adaptador.
//   status_text_test        todos los casos; sale con 1 si alguno falla
#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <cstdio>
#include <string>

#include "status_text.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-60s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

void checkText(const QString &got, const QString &want, const std::string &what) {
    check(got == want, what);
    if (got != want) std::printf("    da:     \"%s\"\n    espera: \"%s\"\n", qPrintable(got), qPrintable(want));
}

RadioStatus watching(double mhz) {
    RadioStatus s;
    s.open = true;
    s.usb3 = true;
    s.chip = QStringLiteral("rtl8812bu");
    s.mode = QStringLiteral("watch");
    s.watchMHz = mhz;
    return s;
}

void osdCases() {
    RadioStatus scan = watching(0);
    scan.mode = QStringLiteral("scan");
    scan.scanStep = 2;
    scan.scanSteps = 10;
    checkText(statustext::osd(scan, nullptr), QStringLiteral("Scanning 3/10"), "OSD escaneando: paso 3 de 10");
    scan.scanSteps = 0;
    checkText(statustext::osd(scan, nullptr), QStringLiteral("Scanning 3/1"), "OSD escaneando sin pasos: de 1");

    RadioStatus w = watching(5769);
    w.snrDb = 19.2;
    w.locked = true;
    w.colour = true;
    w.videoStd = QStringLiteral("NTSC");
    checkText(statustext::osd(w, nullptr), QStringLiteral("R4/A6/B3  5769 MHz  SNR 19 dB  NTSC"),
              "OSD viendo: canal, MHz, SNR y norma");
    w.colour = false;
    checkText(statustext::osd(w, nullptr), QStringLiteral("R4/A6/B3  5769 MHz  SNR 19 dB  NTSC B/W"),
              "OSD viendo en blanco y negro");
    w.locked = false;
    w.snrDb = -1;
    checkText(statustext::osd(w, nullptr), QStringLiteral("R4/A6/B3  5769 MHz  no sync"),
              "OSD sin sincronismo ni SNR");

    Pilot hdz;
    hdz.digital = QStringLiteral("hdzero");
    hdz.freqMHz = 5769;
    checkText(statustext::osd(w, &hdz), QStringLiteral("R4/A6/B3  5769 MHz  HDZero (digital, no picture)"),
              "OSD viendo una senal digital: sin imagen");

    RadioStatus idle = watching(0);
    idle.mode = QStringLiteral("idle");
    checkText(statustext::osd(idle, nullptr), QString(), "OSD parado: nada");
}

void digitalKindCases() {
    Pilot p;
    p.bwMHz = 18;
    p.digital = QStringLiteral("hdzero_narrow");
    checkText(statustext::digitalKind(p), QStringLiteral("HDZero narrow"), "HDZero estrecho");
    p.digital = QStringLiteral("dji");
    checkText(statustext::digitalKind(p), QStringLiteral("DJI 18 MHz"), "DJI con su ancho");
    p.digital = QStringLiteral("otro");
    checkText(statustext::digitalKind(p), QStringLiteral("digital 18 MHz"), "otra senal digital con su ancho");
}

void infoCases() {
    RadioStatus closed;
    closed.rxError = QStringLiteral("x");
    checkText(statustext::info(closed, true), QString(), "info sin adaptador abierto: nada");

    RadioStatus w = watching(5769);
    checkText(statustext::info(w, false), QString(), "info sin modo desarrollador ni avisos: nada");
    w.usb3 = false;
    checkText(statustext::info(w, false), QStringLiteral("WARNING: USB 2, no 40 MS/s. Use a USB 3 port."),
              "info: aviso de USB 2");
    w.chip = QStringLiteral("demo");
    checkText(statustext::info(w, false), QString(), "info: la demo no avisa de USB 2");
    w.usb3 = true;
    w.rxError = QStringLiteral("timeout");
    checkText(statustext::info(w, false), QStringLiteral("error: timeout"), "info: el error, tambien sin desarrollador");

    RadioStatus dev = watching(5769);
    dev.rxState = QStringLiteral("running");
    dev.msps = 20;
    dev.fallback = true;
    dev.lock = 0.954;
    dev.fps = 59.94;
    dev.dropped = 1;
    dev.restarts = 2;
    checkText(statustext::info(dev, true),
              QStringLiteral("chip: rtl8812bu  USB 3\nmode: watch  rx: running\n20 MS/s (fallback)  lock 95%  59.9 fps\n"
                             "dropped: 1  restarts: 2"),
              "info desarrollador viendo: stream, enganche y perdidas");
    dev.mode = QStringLiteral("scan");
    dev.usb3 = false;
    dev.rxError = QStringLiteral("timeout");
    checkText(statustext::info(dev, true),
              QStringLiteral("chip: rtl8812bu  USB 2\nmode: scan  rx: running\ndropped: 1\nerror: timeout\n"
                             "WARNING: USB 2, no 40 MS/s. Use a USB 3 port."),
              "info desarrollador escaneando: perdidas, error y aviso");
    dev.mode = QStringLiteral("other");
    dev.rxError.clear();
    dev.usb3 = true;
    checkText(statustext::info(dev, true), QStringLiteral("chip: rtl8812bu  USB 3\nmode: other  rx: running\ndropped: 1"),
              "info desarrollador: un modo desconocido tal cual");
}
}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // los nombres de canal elegidos se guardan en el .ini: uno vacio, no el del usuario
    const QString ini = QDir::tempPath() + QStringLiteral("/status_text_test");
    QDir(ini).removeRecursively();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, ini);
    osdCases();
    digitalKindCases();
    infoCases();
    std::printf("\n%s\n", failures ? "HAY FALLOS" : "todo bien");
    return failures ? 1 : 0;
}
