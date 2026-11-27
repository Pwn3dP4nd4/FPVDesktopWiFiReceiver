#pragma once
#include <QElapsedTimer>

// El ciclo de vida de un analisis (src/channel_analyzer.h, src/elrs_analyzer.h) frente al barrido del backend, que es
// uno solo para las dos ventanas. Solo decide; la ventana muestra el resultado y emite las ordenes.
//   - Backend::stopSurvey responde siempre con un surveyStopped, en cola: los de los stop() de esta ventana se
//     descuentan (un Parar y Empezar seguidos no se para con la respuesta del primero).
//   - Hasta su primer barrido, un surveyStopped no es para esta ventana: es la respuesta tardia al Parar de la otra
//     (las dos reciben los surveyStopped del backend).
//   - Cualquier otro surveyStopped es otra orden (escanear, ver un canal) que para el analisis en marcha.
class SurveySession {
public:
    bool running() const { return running_; }
    bool adapterOpen() const { return open_; }

    // false: ya estaba asi. *lostRun: se ha perdido el adaptador con el analisis en marcha (se acaba)
    bool setAdapterOpen(bool open, bool *lostRun) {
        *lostRun = false;
        if (open == open_) return false;
        open_ = open;
        if (!open_ && running_) {
            running_ = false;
            awaitingData_ = false;
            *lostRun = true;
        }
        return true;
    }
    // en marcha, esperando su primer barrido; el reloj empieza en 0
    void start() {
        running_ = true;
        awaitingData_ = true;
        clock_.start();
    }
    // Parar a mano. true: estaba en marcha y hay que pedir requestStop (su respuesta se descontara)
    bool stop() {
        if (!running_) return false;
        running_ = false;
        awaitingData_ = false;
        pendingStops_++;
        return true;
    }
    // el otro analizador se queda el barrido (sin pedir nada al backend). true: estaba en marcha
    bool abandon() {
        if (!running_) return false;
        running_ = false;
        awaitingData_ = false;
        return true;
    }
    // ha llegado un barrido para esta ventana
    void onData() { awaitingData_ = false; }
    // surveyStopped del backend. true: otra orden ha parado el analisis en marcha
    bool onStopped() {
        if (pendingStops_ > 0) {
            pendingStops_--;
            return false;
        }
        if (!running_ || awaitingData_) return false;
        running_ = false;
        return true;
    }

    // s desde que empezo el analisis (o el reloj de las pruebas: setTimeForTest, < 0 = el real)
    double now() const {
        if (testTimeS_ >= 0) return testTimeS_;
        return clock_.isValid() ? clock_.elapsed() / 1000.0 : 0;
    }
    void setTimeForTest(double s) { testTimeS_ = s; }

private:
    bool running_ = false, open_ = false;
    bool awaitingData_ = false;
    int pendingStops_ = 0;
    QElapsedTimer clock_;
    double testTimeS_ = -1;
};
