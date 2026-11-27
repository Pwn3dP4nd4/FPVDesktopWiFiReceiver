#pragma once
#include <QString>
#include <QWidget>

class QToolButton;

// Un bloque que se pliega con un boton de cabecera (flecha y titulo), y lo recuerda en el .ini: la leyenda de las
// graficas de los analizadores, que ocupa casi tanto como la grafica y deja de hacer falta cuando ya se conoce.
// El boton se llama "<name>Toggle" y el bloque "<name>" (objectName, para las pruebas). Desplegado por defecto.
class Collapsible : public QWidget {
public:
    Collapsible(QWidget *body, const QString &name, const QString &settingsKey, QWidget *parent = nullptr);
    void setTitle(const QString &title);

private:
    void setOpen(bool open);

    QToolButton *toggle_;
    QWidget *body_;
    QString key_;
    bool open_ = true;
};
