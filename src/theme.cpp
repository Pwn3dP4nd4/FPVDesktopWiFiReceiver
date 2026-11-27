#include "theme.h"
#include "app_settings.h"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

namespace theme {

namespace {
// El aspecto del sistema, tomado antes de cambiarlo por primera vez: para volver a el sin reiniciar.
struct SystemLook {
    bool taken = false;
    QString style;
    QPalette palette;
};
SystemLook g_sys;

// Botones redondeados y planos con el borde de la paleta: la misma hoja para claro y oscuro. Solo botones y ayudas
// emergentes; el resto lo pinta Fusion con la paleta.
const char kSheet[] = R"(
QPushButton { background: palette(button); border: 1px solid palette(mid); border-radius: 5px; padding: 4px 12px; }
QPushButton:hover { border-color: palette(highlight); }
QPushButton:pressed { background: palette(midlight); }
QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }
QPushButton:default { border-color: palette(highlight); }
QToolTip { background: palette(tool-tip-base); color: palette(tool-tip-text); border: 1px solid palette(mid); }
)";

struct Colours {
    QColor window, base, altBase, button, text, disabled, placeholder, highlight, hlText, link;
    QColor light, midlight, mid, dark, shadow;
};

// Oscuro: grises azulados, para mirar video en una pista al atardecer sin deslumbrar. Los campos (Base) algo mas
// claros que la ventana: Fusion bordea las casillas con la ventana oscurecida, y sobre un campo mas oscuro no se
// veian (las de "En el aire" del analizador). El azul de seleccion es el de
// los canales analogicos, algo mas oscuro para que el texto blanco encima se lea (contraste >= 4,5, theme_test).
const Colours kDark{QColor(0x1e2128), QColor(0x262a33), QColor(0x2c313b), QColor(0x2c313b), QColor(0xe6e8ec),
                    QColor(0x6b7280), QColor(0x8a919d), QColor(0x1f6fd6), QColor(0xffffff), QColor(0x58a6ff),
                    QColor(0x4a515e), QColor(0x323844), QColor(0x3a404b), QColor(0x121418), QColor(0x0a0b0d)};
const Colours kLight{QColor(0xf3f4f6), QColor(0xffffff), QColor(0xf6f7f9), QColor(0xfafafa), QColor(0x1f2328),
                     QColor(0x9aa0a8), QColor(0x6b7280), QColor(0x1f6fd6), QColor(0xffffff), QColor(0x1f6fd6),
                     QColor(0xffffff), QColor(0xeceef1), QColor(0xc4c8cf), QColor(0x9aa0a8), QColor(0x6b7280)};

QPalette build(const Colours &c) {
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.altBase);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::ToolTipBase, c.button);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::PlaceholderText, c.placeholder);
    p.setColor(QPalette::Highlight, c.highlight);
    p.setColor(QPalette::HighlightedText, c.hlText);
    p.setColor(QPalette::Link, c.link);
    p.setColor(QPalette::LinkVisited, c.link);
    p.setColor(QPalette::Light, c.light);
    p.setColor(QPalette::Midlight, c.midlight);
    p.setColor(QPalette::Mid, c.mid);
    p.setColor(QPalette::Dark, c.dark);
    p.setColor(QPalette::Shadow, c.shadow);
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, r, c.disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.mid);
    return p;
}

void takeSystemLook() {
    if (g_sys.taken) return;
    g_sys = {true, QApplication::style()->name(), QApplication::palette()};
}
}  // namespace

Theme parse(const QString &s) {
    if (s == QLatin1String("dark")) return Theme::Dark;
    if (s == QLatin1String("light")) return Theme::Light;
    return Theme::System;
}

QString name(Theme t) {
    switch (t) {
    case Theme::Dark: return QStringLiteral("dark");
    case Theme::Light: return QStringLiteral("light");
    case Theme::System: break;
    }
    return QString();
}

Theme setting() { return parse(appSettings().value("theme").toString()); }

QPalette palette(Theme t) {
    switch (t) {
    case Theme::Dark: return build(kDark);
    case Theme::Light: return build(kLight);
    case Theme::System: break;
    }
    return g_sys.taken ? g_sys.palette : QApplication::style()->standardPalette();
}

void apply(Theme t) {
    appSettings().setValue("theme", name(t));
    if (t == Theme::System) {
        if (!g_sys.taken) return;  // nunca se cambio: el aspecto ya es el del sistema
        qApp->setStyleSheet(QString());
        QApplication::setStyle(QStyleFactory::create(g_sys.style));
        QApplication::setPalette(g_sys.palette);
        return;
    }
    takeSystemLook();
    // con hoja de estilo, style() es su envoltorio (sin nombre): se quita antes de mirar el estilo de debajo
    qApp->setStyleSheet(QString());
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setPalette(palette(t));
    qApp->setStyleSheet(QString::fromLatin1(kSheet));
}

}  // namespace theme
