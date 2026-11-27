#include "pilot_card.h"
#include "status_text.h"

#include <algorithm>

namespace pilotcard {

Snr snrLevel(double snrDb) {
    if (snrDb < kLowSnrDb) return Snr::Poor;
    return snrDb < kGoodSnrDb ? Snr::Fair : Snr::Good;
}

double snrFill(double snrDb) { return std::clamp(snrDb / kFullSnrDb, 0.0, 1.0); }

Lines lines(const Pilot &p, const QString &channelLabel) {
    const QString mhz = QStringLiteral("%1 MHz").arg(p.freqMHz, 0, 'f', 0);
    const QString channel = p.isDigital() ? p.name : channelLabel;
    const QString kind = p.isDigital() ? statustext::digitalKind(p) : p.stdName;
    QStringList detail;
    if (!channel.isEmpty()) detail << mhz;
    if (!kind.isEmpty()) detail << kind;
    return {channel.isEmpty() ? mhz : channel, detail.join(QStringLiteral(" · "))};
}

}  // namespace pilotcard
