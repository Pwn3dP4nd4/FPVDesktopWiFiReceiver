#include "status_text.h"

#include <QCoreApplication>

#include "channel_names.h"

namespace statustext {

namespace {
// el contexto de traduccion de siempre (estos textos estaban en MainWindow): lupdate lo toma de la macro
struct T {
    Q_DECLARE_TR_FUNCTIONS(MainWindow)
};

bool is(const RadioStatus &s, const char *mode) { return s.mode == QLatin1String(mode); }

QString watchOsd(const RadioStatus &s, const Pilot *digital) {
    const QString ch = chnames::resolve(s.watchMHz).label();  // "R5/A4/F4" mientras no se elija
    QString osd = QStringLiteral("%1%2 MHz").arg(ch.isEmpty() ? QString() : ch + QStringLiteral("  "))
                      .arg(s.watchMHz, 0, 'f', 0);
    if (s.snrDb >= 0) osd += QStringLiteral("  SNR %1 dB").arg(s.snrDb, 0, 'f', 0);
    if (digital) return osd + QStringLiteral("  ") + T::tr("%1 (digital, no picture)").arg(digitalKind(*digital));
    if (!s.locked) return osd + QStringLiteral("  ") + T::tr("no sync");
    return osd + QStringLiteral("  ") + (s.colour ? s.videoStd : T::tr("%1 B/W").arg(s.videoStd));
}

QString modeName(const RadioStatus &s) {
    if (is(s, "idle")) return T::tr("idle");
    if (is(s, "scan")) return T::tr("scan");
    if (is(s, "watch")) return T::tr("watch");
    if (is(s, "test")) return T::tr("test");
    return s.mode;
}

QString devInfo(const RadioStatus &s) {
    QString t = QStringLiteral("chip: %1  USB %2\n").arg(s.chip, s.usb3 ? "3" : "2");
    t += T::tr("mode: %1  rx: %2").arg(modeName(s), s.rxState) + QLatin1Char('\n');
    // Al escanear, cada cambio de frecuencia reinicia el stream: el contador solo dice algo al ver un canal.
    if (!is(s, "watch")) return t + T::tr("dropped: %1").arg(s.dropped);
    t += T::tr("%1 MS/s%2  lock %3%  %4 fps")
             .arg(s.msps, 0, 'f', 0)
             .arg(s.fallback ? T::tr(" (fallback)") : QString())
             .arg(s.lock * 100, 0, 'f', 0)
             .arg(s.fps, 0, 'f', 1) + QLatin1Char('\n');
    return t + T::tr("dropped: %1  restarts: %2").arg(s.dropped).arg(s.restarts);
}

void addLine(QString &t, const QString &line) { t += (t.isEmpty() ? QString() : QStringLiteral("\n")) + line; }
}  // namespace

QString digitalKind(const Pilot &p) {
    if (p.digital == QLatin1String("hdzero")) return T::tr("HDZero");
    if (p.digital == QLatin1String("hdzero_narrow")) return T::tr("HDZero narrow");
    if (p.digital == QLatin1String("dji")) return T::tr("DJI %1 MHz").arg(p.bwMHz, 0, 'f', 0);
    return T::tr("digital %1 MHz").arg(p.bwMHz, 0, 'f', 0);
}

QString osd(const RadioStatus &s, const Pilot *digital) {
    if (is(s, "scan")) return T::tr("Scanning %1/%2").arg(s.scanStep + 1).arg(qMax(1, s.scanSteps));
    if (is(s, "watch")) return watchOsd(s, digital);
    return QString();
}

QString info(const RadioStatus &s, bool devMode) {
    if (!s.open) return QString();
    QString t = devMode ? devInfo(s) : QString();
    // Errores y avisos: tambien sin el modo desarrollador
    if (!s.rxError.isEmpty()) addLine(t, T::tr("error: %1").arg(s.rxError));
    if (!s.usb3 && s.chip != QLatin1String("demo")) addLine(t, T::tr("WARNING: USB 2, no 40 MS/s. Use a USB 3 port."));
    return t;
}

}  // namespace statustext
