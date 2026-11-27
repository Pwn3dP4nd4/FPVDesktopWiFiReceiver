#pragma once
#include <QSettings>
#include <QString>

// El .ini de la aplicacion: %APPDATA%\FPVDesktopWiFiReceiver (Windows) o ~/.config/FPVDesktopWiFiReceiver (Linux).
// Uno solo por proceso (inline: la misma variable estatica en todas las unidades), compartido por la ventana
// principal, los analizadores y los nombres de canal: lo que escribe uno lo ven los demas.
inline QSettings &appSettings() {
    static QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("FPVDesktopWiFiReceiver"),
                       QStringLiteral("FPVDesktopWiFiReceiver"));
    return s;
}
