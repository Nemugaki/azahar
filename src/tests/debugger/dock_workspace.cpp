// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <cassert>
#include <QApplication>
#include <QPushButton>
#include <QWidget>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include "citra_qt/debugger/dock_workspace.h"

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ads::CDockManager manager;
    ads::CDockWidget dock(&manager, QStringLiteral("Debugger"));
    auto* content = new QWidget;
    dock.setWidget(content);
    dock.setEnabled(false);
    bool callback_active{};
    Debugger::SetDockActiveHandler(&dock,
                                   [&callback_active](bool active) { callback_active = active; });

    Debugger::ConfigureDockWorkspace(&dock);
    manager.addDockWidget(ads::RightDockWidgetArea, &dock);

    assert(dock.features().testFlag(ads::CDockWidget::DockWidgetMovable));
    assert(dock.features().testFlag(ads::CDockWidget::DockWidgetFloatable));
    assert(dock.features().testFlag(ads::CDockWidget::DockWidgetClosable));
    assert(dock.isEnabled());
    assert(dock.property("debuggerWorkspaceContent").value<QObject*>() == content);
    assert(dock.isAncestorOf(content));
    Debugger::SetDockAvailable(&dock, false);
    assert(!content->isEnabled());
    assert(content->updatesEnabled());
    assert(Debugger::IsDockUserEnabled(&dock));

    auto* toggle = qobject_cast<QPushButton*>(
        dock.property("debuggerWorkspaceToggle").value<QObject*>());
    assert(toggle && toggle->isEnabled());
    assert(!toggle->isCheckable());
    toggle->click();
    assert(dock.isEnabled() && toggle->isEnabled() && !content->isEnabled() && !callback_active);
    assert(!Debugger::IsDockUserEnabled(&dock));

    Debugger::SetDockAvailable(&dock, true);
    toggle->click();
    assert(Debugger::IsDockActive(&dock) && content->isEnabled() && callback_active);

    dock.setFloating();
    app.processEvents();
    assert(dock.isEnabled());
    assert(dock.isInFloatingContainer());

    ads::CDockManager::setConfigFlag(ads::CDockManager::EqualSplitOnInsertion, true);
    ads::CDockManager layout_manager;
    layout_manager.resize(1000, 700);
    auto make_dock = [&layout_manager](const QString& title) {
        auto* widget = new ads::CDockWidget(&layout_manager, title);
        widget->setWidget(new QWidget);
        return widget;
    };
    auto* center = layout_manager.setCentralWidget(make_dock(QStringLiteral("Center")));
    auto* game = make_dock(QStringLiteral("Game"));
    layout_manager.addDockWidget(ads::CenterDockWidgetArea, game, center);
    game->toggleView(false);
    game->toggleView(true);
    auto* left = layout_manager.addDockWidget(ads::LeftDockWidgetArea,
                                               make_dock(QStringLiteral("Left")));
    auto* right = layout_manager.addDockWidget(ads::RightDockWidgetArea,
                                                make_dock(QStringLiteral("Right")));
    layout_manager.show();
    app.processEvents();
    assert(game->dockAreaWidget() == center);
    assert(game->height() > 0);
    assert(center->height() > 0);
    assert(left->geometry().top() == center->geometry().top());
    assert(left->geometry().bottom() == center->geometry().bottom());
    assert(right->geometry().top() == center->geometry().top());
    assert(right->geometry().bottom() == center->geometry().bottom());

    auto* bottom = layout_manager.addDockWidget(ads::BottomDockWidgetArea,
                                                 make_dock(QStringLiteral("Bottom")), center);
    app.processEvents();
    assert(center->height() > 200);
    assert(bottom->height() > 200);
    assert(bottom->geometry().top() > center->geometry().bottom());
    return 0;
}
