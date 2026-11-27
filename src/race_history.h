#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

#include <vector>

#include "race_monitor.h"

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QFont;

// Historial de las carreras de una ventana del analizador (src/channel_analyzer.h, pestana Historial): lo que ha ido
// avisando y recomendando el seguimiento, los cambios aplicados y el resumen final. Por piloto (cual ha tenido mas
// problemas y cuales, por categoria) o todo seguido; se copia o se guarda en CSV / texto.
class RaceHistory : public QWidget {
    Q_OBJECT
public:
    // Problem: su canal tiene un problema y no hay otro libre mejor (sin canal mejor); Move: recomendado moverlo.
    // Start: empieza una carrera (o vuelve a empezar con otros fijados): se marca en la vista cronologica
    enum class Ev { Info, Start, Ok, Warn, Problem, Move, Applied, Summary };
    // de un vistazo, por piloto: graves (su canal le da problemas: mover o sin canal mejor), avisos (video flojo que no
    // es del canal, una senal que no se va), cambios de canal aplicados e informacion (OK, empezar, parar, resumen)
    enum class Cat { Serious, Warning, Change, Info };
    static Cat category(Ev k);

    explicit RaceHistory(QWidget *parent = nullptr);

    // Empieza una carrera: un piloto por canal fijado (por orden), con su canal (channels[i]); el reloj, a 0
    void beginRace(const std::vector<race::ChannelHealth> &pilots, const QStringList &channels);
    // como le va a cada piloto de la carrera en marcha (race::Monitor::health(); tras moverlo, solo su canal nuevo)
    void syncHealth(const std::vector<race::ChannelHealth> &health);
    // el piloto de la carrera en marcha ha pasado a este canal (Aplicar)
    void pilotMoved(int pilot, const QString &channel);
    // pilot: indice en la carrera en marcha (= fijado); -1 = de toda la carrera. sweep: race::Monitor::sweeps()
    void addEvent(Ev kind, int sweep, const QString &channel, const QString &what, const QString &detail = QString(),
                  int pilot = -1);
    // Borrar: los eventos y las carreras terminadas; keepCurrent: la ultima sigue (sus eventos nuevos van a ella)
    void clear(bool keepCurrent);
    int eventCount() const { return events_.size(); }
    // la tabla por piloto otra vez (su video, en vivo, con la pestana a la vista)
    void refreshPilots() { rebuildPilotView(); }

    // SNR, calidad, cortes...; summary: con los barridos (para el resumen)
    static QString healthText(const race::ChannelHealth &h, bool summary);


signals:
    void eventsChanged(int count);  // para el titulo de la pestana
    void clearRequested();          // el boton Borrar: quien lo tiene sabe si hay una carrera en marcha

protected:
    void changeEvent(QEvent *e) override;

private:
    struct RaceEvent {
        QDateTime when;
        double raceS = 0;  // s desde que empezo el seguimiento
        int sweep = 0;
        Ev kind = Ev::Info;
        QString channel, what, detail;
        int race = 0;    // numero de carrera (races_), 0 = fuera de ninguna
        int pilot = -1;  // indice del piloto en esa carrera; -1 = de toda la carrera
    };
    // un piloto por canal fijado al empezar (por orden), los canales por los que ha pasado (Aplicar) y como le ha ido
    struct PilotRec {
        QStringList channels;
        race::ChannelHealth health;
    };
    struct RaceRec {
        int no = 0;
        QDateTime start;
        QVector<PilotRec> pilots;
    };

    void retranslate();
    QString catName(Cat c) const;
    const RaceRec *raceRec(int no) const;
    QString pilotLabel(int race, int pilot, bool withRace) const;  // "Pilot 2 (R3 → R2)"
    int histRace() const;  // la carrera elegida arriba; 0 = todas
    bool histShows(const RaceEvent &e) const;
    void addHistRow(const RaceEvent &e);  // a la vista cronologica
    void rebuild();  // las dos vistas (cambia la carrera elegida, Borrar, idioma)
    // una fila de la vista por piloto: cuantos eventos de cada categoria (Cat) y como le fue
    struct PilotRow {
        int race = 0, pilot = -1;  // pilot -1: lo de toda la carrera
        int n[4] = {0, 0, 0, 0};
        const PilotRec *rec = nullptr;
        // no se le ha visto nunca (VTX apagado o roto): no ha tenido video, cuenta como un grave
        bool neverSeen() const { return rec && rec->health.sweeps > 0 && rec->health.seen == 0; }
        int score() const { return n[0] * 3 + n[1] + (neverSeen() ? 3 : 0); }  // cuantos problemas
    };
    QVector<PilotRow> pilotRows() const;  // de la carrera elegida, de mas a menos problemas
    QTreeWidgetItem *addPilotRow(const PilotRow &r, bool withRace, const QFont &bold);
    QString verdictText(const QVector<PilotRow> &rows, bool withRace) const;  // "Most trouble: ..."
    void rebuildPilotView();
    void showPilotEvents();  // eventos del piloto elegido, por categoria
    void countChanged();     // los botones (sin eventos no hay nada que copiar) y eventsChanged
    QString historyText(bool csv) const;
    void save();

    QVector<RaceEvent> events_;  // de todas las carreras de esta ventana (Borrar las quita)
    QVector<RaceRec> races_;
    QElapsedTimer raceClock_;
    QComboBox *histRace_ = nullptr, *histView_ = nullptr;
    QLabel *histRaceLbl_ = nullptr, *histVerdict_ = nullptr, *pilotInfo_ = nullptr;
    QStackedWidget *histStack_ = nullptr;
    QTreeWidget *pilotTable_ = nullptr, *pilotEvents_ = nullptr;
    bool pilotPicked_ = false;  // el usuario ha elegido un piloto (si no, se muestra el de mas problemas)
    int shownRace_ = -1, shownPilot_ = -2;  // el piloto de pilotEvents_ (al rehacerlo, conservar lo abierto)
    QTreeWidget *histTable_ = nullptr;
    QLabel *histHint_ = nullptr;
    QPushButton *histCopy_ = nullptr, *histSave_ = nullptr, *histClear_ = nullptr;
};
