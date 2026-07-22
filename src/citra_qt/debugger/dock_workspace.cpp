// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <QDockWidget>
#include <QHBoxLayout>
#include <QLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include "citra_qt/debugger/dock_workspace.h"

namespace Debugger {
namespace {

constexpr auto ContentProperty = "debuggerWorkspaceContent";
constexpr auto ToggleProperty = "debuggerWorkspaceToggle";
constexpr auto EnabledProperty = "debuggerWorkspaceEnabled";
constexpr auto AvailableProperty = "debuggerWorkspaceAvailable";
constexpr auto HandlerProperty = "debuggerWorkspaceHandler";

class ActiveHandler final : public QObject {
public:
    ActiveHandler(QDockWidget* dock, std::function<void(bool)> callback_)
        : QObject(dock), callback{std::move(callback_)} {}

    std::function<void(bool)> callback;
};

void ApplyDockState(QDockWidget* dock) {
    auto* content = static_cast<QWidget*>(dock->property(ContentProperty).value<QObject*>());
    auto* toggle = static_cast<QPushButton*>(dock->property(ToggleProperty).value<QObject*>());
    if (!content || !toggle) {
        return;
    }

    dock->setEnabled(true);
    toggle->parentWidget()->setEnabled(true);
    toggle->setEnabled(true);

    const bool user_enabled = dock->property(EnabledProperty).toBool();
    const bool active = user_enabled && dock->property(AvailableProperty).toBool();
    dock->widget()->setEnabled(true);
    toggle->setEnabled(true);
    content->setEnabled(active);

    auto* handler = static_cast<ActiveHandler*>(dock->property(HandlerProperty).value<QObject*>());
    if (handler && handler->callback) {
        handler->callback(active);
    }

    toggle->setText(user_enabled ? QObject::tr("Disable") : QObject::tr("Enable"));
    toggle->setToolTip(user_enabled
                           ? QObject::tr("Disable this debugger without changing the workspace")
                           : QObject::tr("Enable this debugger without changing the workspace"));
}

} // namespace

void ConfigureDockWorkspace(QDockWidget* dock) {
    if (!dock || dock->property(ContentProperty).isValid()) {
        return;
    }

    QWidget* content = dock->widget();
    if (!content) {
        return;
    }

    const bool available = dock->isEnabled();
    content->setParent(nullptr);
    dock->setWidget(nullptr);
    dock->setEnabled(true);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setMinimumSize(1, 1);
    content->setMinimumSize(0, 0);

    auto* container = new QWidget(dock);
    auto* layout = new QVBoxLayout(container);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto* controls = new QHBoxLayout;
    auto* toggle = new QPushButton(QObject::tr("Disable"), container);
    toggle->setAccessibleName(QObject::tr("Enable or disable %1").arg(dock->windowTitle()));
    controls->addStretch();
    controls->addWidget(toggle);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(controls);
    layout->addWidget(content);
    dock->setWidget(container);
    container->setEnabled(true);
    toggle->setEnabled(true);

    dock->setProperty(ContentProperty, QVariant::fromValue(static_cast<QObject*>(content)));
    dock->setProperty(ToggleProperty, QVariant::fromValue(static_cast<QObject*>(toggle)));
    dock->setProperty(EnabledProperty, true);
    dock->setProperty(AvailableProperty, available);

    QObject::connect(toggle, &QPushButton::clicked, dock, [dock] {
        dock->setProperty(EnabledProperty, !dock->property(EnabledProperty).toBool());
        ApplyDockState(dock);
    });
    ApplyDockState(dock);
}

void SetDockAvailable(QDockWidget* dock, bool available) {
    if (!dock || !dock->property(ContentProperty).isValid()) {
        if (dock) {
            dock->setEnabled(available);
        }
        return;
    }
    dock->setProperty(AvailableProperty, available);
    ApplyDockState(dock);
}

bool IsDockActive(const QDockWidget* dock) {
    return dock && dock->property(EnabledProperty).toBool() &&
           dock->property(AvailableProperty).toBool();
}

bool IsDockUserEnabled(const QDockWidget* dock) {
    return dock && dock->property(EnabledProperty).toBool();
}

void SetDockActiveHandler(QDockWidget* dock, std::function<void(bool)> callback) {
    if (!dock) {
        return;
    }
    if (auto* old = static_cast<QObject*>(dock->property(HandlerProperty).value<QObject*>())) {
        delete old;
    }
    auto* handler = new ActiveHandler(dock, std::move(callback));
    dock->setProperty(HandlerProperty, QVariant::fromValue(static_cast<QObject*>(handler)));
    if (dock->property(ContentProperty).isValid()) {
        ApplyDockState(dock);
    }
}

} // namespace Debugger
