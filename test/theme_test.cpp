// Tema de la interfaz (src/theme.h): el ajuste guardado, que las paletas clara y oscura se lean (contraste) y que
// aplicarlo cambie el aspecto de la aplicacion y se pueda volver al del sistema. Sin adaptador.
//   theme_test        sale con 1 si algo falla (sin pantalla: offscreen)
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStyle>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app_settings.h"
#include "theme.h"

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%-64s %s\n", what.c_str(), ok ? "ok" : "FALLO");
    if (!ok) failures++;
}

// contraste WCAG 2 entre dos colores (1..21)
double luminance(const QColor &c) {
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}
double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void settingCases() {
    check(theme::parse(QStringLiteral("dark")) == theme::Theme::Dark, "ajuste: dark");
    check(theme::parse(QStringLiteral("light")) == theme::Theme::Light, "ajuste: light");
    check(theme::parse(QString()) == theme::Theme::System, "ajuste: vacio = sistema");
    check(theme::parse(QStringLiteral("rosa")) == theme::Theme::System, "ajuste: desconocido = sistema");
    for (theme::Theme t : {theme::Theme::System, theme::Theme::Light, theme::Theme::Dark})
        check(theme::parse(theme::name(t)) == t, "ajuste: ida y vuelta " + theme::name(t).toStdString());
}

// texto normal: al menos 4,5:1 (WCAG AA) en cada pareja que pinta un widget
void readable(theme::Theme t, const std::string &label) {
    const QPalette p = theme::palette(t);
    const struct { QPalette::ColorRole fg, bg; const char *what; } pairs[] = {
        {QPalette::WindowText, QPalette::Window, "texto sobre ventana"},
        {QPalette::Text, QPalette::Base, "texto sobre campo"},
        {QPalette::ButtonText, QPalette::Button, "texto de boton"},
        {QPalette::HighlightedText, QPalette::Highlight, "texto seleccionado"},
        {QPalette::ToolTipText, QPalette::ToolTipBase, "ayuda emergente"},
    };
    for (const auto &pr : pairs)
        check(contrast(p.color(pr.fg), p.color(pr.bg)) >= 4.5, label + ": " + pr.what + " legible");
    check(contrast(p.color(QPalette::Disabled, QPalette::WindowText), p.color(QPalette::Window)) >= 2.0,
          label + ": desactivado se distingue del fondo");
    check(p.color(QPalette::Disabled, QPalette::WindowText) != p.color(QPalette::WindowText),
          label + ": desactivado se distingue del activo");
}

void paletteCases() {
    readable(theme::Theme::Light, "claro");
    readable(theme::Theme::Dark, "oscuro");
    check(theme::palette(theme::Theme::Dark).color(QPalette::Window).lightness() < 64, "oscuro: fondo oscuro");
    check(theme::palette(theme::Theme::Light).color(QPalette::Window).lightness() > 192, "claro: fondo claro");
}

void applyCases() {
    const QString sysStyle = QApplication::style()->name();
    const QColor sysWindow = QApplication::palette().color(QPalette::Window);

    theme::apply(theme::Theme::Dark);
    check(appSettings().value("theme").toString() == QLatin1String("dark"), "aplicar oscuro: se guarda");
    check(theme::setting() == theme::Theme::Dark, "aplicar oscuro: se lee al volver a abrir");
    check(QApplication::palette().color(QPalette::Window) == theme::palette(theme::Theme::Dark).color(QPalette::Window),
          "aplicar oscuro: paleta oscura");
    check(!qApp->styleSheet().isEmpty(), "aplicar oscuro: hoja de estilo");

    theme::apply(theme::Theme::Light);
    check(QApplication::palette().color(QPalette::Window) ==
              theme::palette(theme::Theme::Light).color(QPalette::Window),
          "aplicar claro: paleta clara");

    theme::apply(theme::Theme::System);
    check(appSettings().value("theme").toString().isEmpty(), "volver al sistema: se guarda");
    check(QApplication::style()->name() == sysStyle, "volver al sistema: su estilo");
    check(QApplication::palette().color(QPalette::Window) == sysWindow, "volver al sistema: su paleta");
    check(qApp->styleSheet().isEmpty(), "volver al sistema: sin hoja de estilo");
}
}  // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // un .ini propio en la carpeta temporal: no toca el del usuario
    const QString ini = QDir::tempPath() + QStringLiteral("/theme_test");
    QDir(ini).removeRecursively();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, ini);
    settingCases();
    paletteCases();
    applyCases();
    std::printf("\n%s (%d fallo(s))\n", failures ? "FALLA" : "OK", failures);
    return failures ? 1 : 0;
}
