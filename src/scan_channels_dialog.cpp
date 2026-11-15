#include "scan_channels_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "scan_plan.h"
#include "spectrum.h"  // kp::fpv_channels()

namespace {
QString bandTitle(QChar b) {
    switch (b.toLatin1()) {
        case 'A': return QStringLiteral("A  Boscam A");
        case 'B': return QStringLiteral("B  Boscam B");
        case 'E': return QStringLiteral("E  Boscam E");
        case 'F': return QStringLiteral("F  Fatshark");
        case 'R': return QStringLiteral("R  Raceband");
        case 'L': return ScanChannelsDialog::tr("L  Low band");
        default: return QString(b);
    }
}
}  // namespace

QVector<double> ScanChannelsDialog::frequencies(const QStringList &names) {
    QVector<double> out;
    for (const auto &c : kp::fpv_channels())
        if (names.contains(QLatin1String(c.name))) out.push_back(c.mhz);
    return out;
}

ScanChannelsDialog::ScanChannelsDialog(const QStringList &selected, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Channels to scan"));
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Scan only the channels your VTX may be on: every 20 MHz step skipped saves "
                                "about a second. With every channel selected the full range is swept, "
                                "including non-standard frequencies."));
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    QChar cur;
    for (const auto &c : kp::fpv_channels()) {
        const QString name = QString::fromLatin1(c.name);
        if (name.at(0) != cur) {
            cur = name.at(0);
            Band b;
            b.box = new QCheckBox(bandTitle(cur));
            b.box->setStyleSheet(QStringLiteral("QCheckBox{font-weight:bold;}"));
            const int band = bands_.size();
            connect(b.box, &QCheckBox::clicked, this, [this, band] {
                bool all = true;
                for (auto *ch : bands_[band].channels) all = all && ch->isChecked();
                for (auto *ch : bands_[band].channels) ch->setChecked(!all);
                refresh();
            });
            grid->addWidget(b.box, band, 0);
            bands_.push_back(b);
        }
        auto *ch = new QCheckBox(QStringLiteral("%1 %2").arg(name).arg(c.mhz, 0, 'f', 0));
        ch->setProperty("channel", name);
        ch->setChecked(selected.isEmpty() || selected.contains(name));
        connect(ch, &QCheckBox::toggled, this, &ScanChannelsDialog::refresh);
        Band &b = bands_.last();
        grid->addWidget(ch, bands_.size() - 1, 1 + b.channels.size());
        b.channels.push_back(ch);
    }
    v->addLayout(grid);

    summary_ = new QLabel;
    v->addWidget(summary_);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box->addButton(tr("All"), QDialogButtonBox::ResetRole), &QPushButton::clicked, this, [this] { setAll(true); });
    connect(box->addButton(tr("None"), QDialogButtonBox::ResetRole), &QPushButton::clicked, this, [this] { setAll(false); });
    ok_ = box->button(QDialogButtonBox::Ok);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(box);
    refresh();
}

void ScanChannelsDialog::setAll(bool on) {
    for (auto &b : bands_)
        for (auto *ch : b.channels) {
            const QSignalBlocker block(ch);
            ch->setChecked(on);
        }
    refresh();
}

void ScanChannelsDialog::refresh() {
    int total = 0, on = 0;
    for (auto &b : bands_) {
        int n = 0;
        for (auto *ch : b.channels) n += ch->isChecked();
        b.box->setCheckState(n == 0 ? Qt::Unchecked : n == b.channels.size() ? Qt::Checked : Qt::PartiallyChecked);
        total += b.channels.size();
        on += n;
    }
    const QVector<double> mhz = frequencies(selected());
    const int steps = on == total ? full_scan_steps() : int(scan_plan_for(std::vector<double>(mhz.begin(), mhz.end())).size());
    summary_->setText(on == total ? tr("All %1 channels: full sweep, %2 steps.").arg(total).arg(steps)
                                  : tr("%1 of %2 channels: %3 of %4 steps.").arg(on).arg(total).arg(steps).arg(full_scan_steps()));
    ok_->setEnabled(on > 0);
}

QStringList ScanChannelsDialog::selected() const {
    QStringList out;
    bool all = true;
    for (const auto &b : bands_)
        for (auto *ch : b.channels) {
            if (ch->isChecked()) out << ch->property("channel").toString();
            else all = false;
        }
    return all ? QStringList() : out;
}
