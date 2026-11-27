#include "collapsible.h"
#include "app_settings.h"

#include <QToolButton>
#include <QVBoxLayout>

Collapsible::Collapsible(QWidget *body, const QString &name, const QString &settingsKey, QWidget *parent)
    : QWidget(parent), toggle_(new QToolButton), body_(body), key_(settingsKey) {
    body_->setObjectName(name);
    toggle_->setObjectName(name + QStringLiteral("Toggle"));
    toggle_->setAutoRaise(true);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(toggle_, 0, Qt::AlignLeft);
    v->addWidget(body_);
    setOpen(appSettings().value(key_, true).toBool());
    // no checkable: con autoRaise, un boton marcado se ve hundido
    connect(toggle_, &QToolButton::clicked, this, [this] {
        appSettings().setValue(key_, !open_);
        setOpen(!open_);
    });
}

void Collapsible::setTitle(const QString &title) { toggle_->setText(title); }

void Collapsible::setOpen(bool open) {
    open_ = open;
    toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    body_->setVisible(open);
}
