#pragma once
#include <QDialog>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QLabel;
class QPushButton;

// Elegir que canales estandar escanea "Escanear" (bandas A, B, E, F, R, L de kp::fpv_channels()).
// Nombres de canal ("R4"); lista vacia = todos (plan completo de kepyur, tambien frecuencias no estandar).
class ScanChannelsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScanChannelsDialog(const QStringList &selected, QWidget *parent = nullptr);
    QStringList selected() const;  // vacia = todos

    static QVector<double> frequencies(const QStringList &names);  // MHz de cada canal conocido

private:
    struct Band {
        QCheckBox *box;
        QVector<QCheckBox *> channels;
    };
    void setAll(bool on);
    void refresh();
    QVector<Band> bands_;
    QLabel *summary_;
    QPushButton *ok_;
};
