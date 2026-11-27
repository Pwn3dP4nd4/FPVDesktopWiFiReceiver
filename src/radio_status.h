#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QVector>

#include "backend.h"

// Lo que dice fpv::Radio::status_json(), para la GUI: se lee aqui, donde entra (src/radio_backend.cpp lo consulta
// cada 250 ms); dentro, RadioStatus y Pilot.
namespace radio {

RadioStatus parseStatus(const QJsonObject &o);
QVector<Pilot> parsePilots(const QJsonObject &o);  // "pilots"
// lo que se ve de la lista de pilotos (sin la edad, que sube cada segundo): solo se avisa a la GUI si cambia
QByteArray pilotsKey(const QVector<Pilot> &pilots);

}  // namespace radio
