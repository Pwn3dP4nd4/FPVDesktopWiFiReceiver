#pragma once
#include <QString>

// Idioma de la interfaz. Los textos del codigo estan en ingles; el espanol va en
// translations/FPVDesktopWiFiReceiver_es.ts, compilado (.qm) dentro del ejecutable.
namespace i18n {

// Ajuste guardado: "" = idioma del sistema, "en", "es".
QString setting();
// Idioma efectivo ("en" / "es") para un ajuste.
QString resolve(const QString &setting);
// Carga los traductores del idioma (los de la app y los de Qt para dialogos estandar) y guarda el ajuste.
// Los widgets reciben QEvent::LanguageChange y se retraducen al momento.
void apply(const QString &setting);

}  // namespace i18n
