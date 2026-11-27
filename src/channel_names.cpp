#include "channel_names.h"
#include "app_settings.h"

#include <QSettings>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "backend.h"   // Pilot
#include "spectrum.h"  // kp::fpv_channels(), kp::nearest_channel()

namespace chnames {

namespace {
const char kKey[] = "channelChoices";
const char kOrder[] = "RFABEL";  // como kepyur: Raceband primero, es con mucho la mas usada

QStringList &choices() {
    static QStringList c = appSettings().value(kKey).toStringList();
    return c;
}

int bandRank(const char *name) {
    const char *p = std::strchr(kOrder, name[0]);
    return p ? int(p - kOrder) : int(sizeof kOrder);
}

bool hdzeroChannel(const char *name) {
    const QString n = QString::fromLatin1(name);
    return n[0] == QLatin1Char('R') || n[0] == QLatin1Char('L') || n == QLatin1String("E1") ||
           n == QLatin1String("F1") || n == QLatin1String("F2") || n == QLatin1String("F4");
}
}  // namespace

double channelMHz(const QString &name) {
    for (const auto &c : kp::fpv_channels())
        if (name == QLatin1String(c.name)) return c.mhz;
    return 0;
}

Resolved resolve(double mhz, bool digital) {
    Resolved r;
    if (mhz <= 0) return r;
    if (digital) {
        const kp::FpvChannel *best = nullptr;
        double bd = 3.0;
        for (const auto &c : kp::fpv_channels()) {
            const double d = std::fabs(c.mhz - mhz);
            if (hdzeroChannel(c.name) && d < bd) {
                bd = d;
                best = &c;
            }
        }
        if (best) r.name = QString::fromLatin1(best->name);
        return r;
    }
    std::vector<const kp::FpvChannel *> near;
    for (const auto &c : kp::fpv_channels())
        if (std::fabs(c.mhz - mhz) <= kWindowMHz) near.push_back(&c);
    if (near.empty()) {  // lejos de todos: el mas cercano a 6 MHz como antes, sin alternativas
        if (const auto *c = kp::nearest_channel(mhz)) r.name = QString::fromLatin1(c->name);
        return r;
    }
    // orden fijo (banda y numero), no por distancia: el texto no cambia de un barrido a otro
    std::stable_sort(near.begin(), near.end(), [](const kp::FpvChannel *a, const kp::FpvChannel *b) {
        const int ra = bandRank(a->name), rb = bandRank(b->name);
        return ra != rb ? ra < rb : std::strcmp(a->name, b->name) < 0;
    });
    // el elegido por el usuario entre estos (el mas cercano si hubiera dos)
    const kp::FpvChannel *pick = nullptr;
    for (const auto *c : near)
        if (choices().contains(QLatin1String(c->name)) &&
            (!pick || std::fabs(c->mhz - mhz) < std::fabs(pick->mhz - mhz)))
            pick = c;
    r.chosen = pick != nullptr;
    if (!pick) pick = near.front();
    r.name = QString::fromLatin1(pick->name);
    if (near.size() > 1) {
        r.alternatives << r.name;
        for (const auto *c : near)
            if (c != pick) r.alternatives << QString::fromLatin1(c->name);
    }
    return r;
}

void apply(Pilot &p) {
    const Resolved r = resolve(p.freqMHz, p.isDigital());
    p.name = r.name;
    p.altNames = r.alternatives;
    p.nameChosen = r.chosen;
}

void choose(const QString &name) {
    const double f = channelMHz(name);
    if (f <= 0) return;
    // fuera las elecciones entre los que se confunden con este (a 2 x kWindowMHz: los que pueden salir juntos)
    QStringList &c = choices();
    c.erase(std::remove_if(c.begin(), c.end(), [&](const QString &n) {
                const double g = channelMHz(n);
                return g <= 0 || std::fabs(g - f) <= 2 * kWindowMHz;
            }),
            c.end());
    c << name;
    appSettings().setValue(kKey, c);
}

}  // namespace chnames
