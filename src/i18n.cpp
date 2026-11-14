#include "i18n.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

#include "main_window.h"

namespace i18n {

namespace {
QTranslator *g_app = nullptr;
QTranslator *g_qt = nullptr;

void drop(QTranslator *&t) {
    if (!t) return;
    QCoreApplication::removeTranslator(t);
    delete t;
    t = nullptr;
}
}  // namespace

QString setting() {
    const QString s = MainWindow::settings().value("language").toString();
    return s == QLatin1String("en") || s == QLatin1String("es") ? s : QString();
}

QString resolve(const QString &s) {
    if (!s.isEmpty()) return s;
    return QLocale::system().language() == QLocale::Spanish ? QStringLiteral("es") : QStringLiteral("en");
}

void apply(const QString &s) {
    MainWindow::settings().setValue("language", s);
    const QString lang = resolve(s);
    drop(g_app);
    drop(g_qt);
    if (lang == QLatin1String("en")) {  // el idioma de los textos fuente: sin traductor
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        // Aun asi se instala uno vacio para que los widgets reciban LanguageChange al volver a ingles.
        g_app = new QTranslator;
        QCoreApplication::installTranslator(g_app);
        return;
    }
    QLocale::setDefault(QLocale(lang));
    g_app = new QTranslator;
    if (g_app->load(QStringLiteral(":/i18n/FPVDesktopWiFiReceiver_%1.qm").arg(lang)))
        QCoreApplication::installTranslator(g_app);
    // Botones y dialogos de Qt (OK, Cancelar...): junto al .exe (windeployqt) o los del Qt instalado.
    // windeployqt los fusiona en qt_<lang>.qm; una instalacion de Qt trae qtbase_<lang>.qm.
    g_qt = new QTranslator;
    bool ok = false;
    for (const QString &dir : {QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
                               QLibraryInfo::path(QLibraryInfo::TranslationsPath)}) {
        for (const QString &base : {QStringLiteral("qt_"), QStringLiteral("qtbase_")})
            if ((ok = g_qt->load(base + lang, dir))) break;
        if (ok) break;
    }
    if (ok)
        QCoreApplication::installTranslator(g_qt);
    else
        drop(g_qt);
}

}  // namespace i18n
