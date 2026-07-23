// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <QApplication>
#include <QPushButton>
#include <QWidget>
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>
#include "citra_qt/debugger/dock_workspace.h"

#define CHECK(condition)                                                                            \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            return __LINE__;                                                                        \
        }                                                                                           \
    } while (false)

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

    CHECK(dock.features().testFlag(ads::CDockWidget::DockWidgetMovable));
    CHECK(dock.features().testFlag(ads::CDockWidget::DockWidgetFloatable));
    CHECK(dock.features().testFlag(ads::CDockWidget::DockWidgetClosable));
    CHECK(dock.isEnabled());
    CHECK(dock.property("debuggerWorkspaceContent").value<QObject*>() == content);
    CHECK(dock.isAncestorOf(content));
    Debugger::SetDockAvailable(&dock, false);
    CHECK(!content->isEnabled());
    CHECK(content->updatesEnabled());
    CHECK(Debugger::IsDockUserEnabled(&dock));

    auto* toggle = qobject_cast<QPushButton*>(
        dock.property("debuggerWorkspaceToggle").value<QObject*>());
    CHECK(toggle && toggle->isEnabled());
    CHECK(!toggle->isCheckable());
    toggle->click();
    CHECK(dock.isEnabled() && toggle->isEnabled() && !content->isEnabled() && !callback_active);
    CHECK(!Debugger::IsDockUserEnabled(&dock));

    Debugger::SetDockAvailable(&dock, true);
    toggle->click();
    CHECK(Debugger::IsDockActive(&dock) && content->isEnabled() && callback_active);

    dock.setFloating();
    app.processEvents();
    CHECK(dock.isEnabled());
    CHECK(dock.isInFloatingContainer());

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
    CHECK(game->dockAreaWidget() == center);
    CHECK(game->height() > 0);
    CHECK(center->height() > 0);
    CHECK(left->geometry().top() == center->geometry().top());
    CHECK(left->geometry().bottom() == center->geometry().bottom());
    CHECK(right->geometry().top() == center->geometry().top());
    CHECK(right->geometry().bottom() == center->geometry().bottom());

    auto* bottom = layout_manager.addDockWidget(ads::BottomDockWidgetArea,
                                                 make_dock(QStringLiteral("Bottom")), center);
    app.processEvents();
    CHECK(center->height() > 200);
    CHECK(bottom->height() > 200);
    CHECK(bottom->geometry().top() > center->geometry().bottom());
    return 0;
}
