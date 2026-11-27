#include "radio_status.h"

#include <QJsonArray>

namespace radio {

RadioStatus parseStatus(const QJsonObject &o) {
    RadioStatus s;
    s.open = o.value("open").toBool();
    s.usb3 = o.value("usb3").toBool();
    s.chip = o.value("chip").toString();
    s.mode = o.value("mode").toString();
    s.rxState = o.value("rx_state").toString();
    s.rxError = o.value("rx_error").toString();
    s.watchMHz = o.value("watch_hz").toDouble() / 1e6;
    s.msps = o.value("watch_msps").toDouble();
    s.fallback = o.value("fallback").toBool();
    s.snrDb = o.contains("snr_db") ? o.value("snr_db").toDouble() : -1;
    s.scanStep = o.value("scan_step").toInt();
    s.scanSteps = o.value("scan_steps").toInt();
    const QJsonObject v = o.value("video").toObject();
    s.locked = v.value("locked").toBool();
    s.lock = v.value("lock").toDouble();
    s.videoStd = v.value("std").toString();
    s.fps = v.value("fps").toDouble();
    s.colour = v.value("colour").toBool();
    s.dropped = static_cast<quint64>(o.value("dropped").toDouble());
    s.restarts = static_cast<quint64>(o.value("restarts").toDouble());
    return s;
}

QVector<Pilot> parsePilots(const QJsonObject &o) {
    QVector<Pilot> ps;
    for (const auto &e : o.value("pilots").toArray()) {
        const QJsonObject p = e.toObject();
        Pilot x;
        x.freqMHz = p.value("mhz").toDouble();
        x.snrDb = p.value("snr").toDouble();
        x.name = p.value("name").toString();
        x.stdName = p.value("std").toString();
        x.ageS = p.value("age_s").toInt();
        x.digital = p.value("digital").toString();  // cmake/kepyur_patches.cmake, conjunto "digital"
        x.bwMHz = p.value("bw").toDouble();
        ps.push_back(x);
    }
    return ps;
}

QByteArray pilotsKey(const QVector<Pilot> &pilots) {
    QByteArray key;
    for (const Pilot &x : pilots)
        key += QStringLiteral("%1|%2|%3|%4|%5;")
                   .arg(x.freqMHz, 0, 'f', 1)
                   .arg(x.snrDb, 0, 'f', 1)
                   .arg(x.name, x.stdName, x.digital)
                   .toUtf8();
    return key;
}

}  // namespace radio
