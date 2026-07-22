// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <cassert>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QWidget>
#include "citra_qt/debugger/dock_workspace.h"

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QMainWindow window;
    QDockWidget dock(QStringLiteral("Debugger"), &window);
    auto* content = new QWidget;
    dock.setWidget(content);
    dock.setEnabled(false);
    bool callback_active{};
    Debugger::SetDockActiveHandler(&dock,
                                   [&callback_active](bool active) { callback_active = active; });

    window.addDockWidget(Qt::RightDockWidgetArea, &dock);
    Debugger::ConfigureDockWorkspace(&dock);

    assert(dock.allowedAreas() == Qt::AllDockWidgetAreas);
    assert(dock.features().testFlag(QDockWidget::DockWidgetMovable));
    assert(dock.features().testFlag(QDockWidget::DockWidgetFloatable));
    assert(dock.features().testFlag(QDockWidget::DockWidgetClosable));
    assert(dock.isEnabled());
    assert(dock.property("debuggerWorkspaceContent").value<QObject*>() == content);
    assert(dock.isAncestorOf(content));
    Debugger::SetDockAvailable(&dock, false);
    assert(!content->isEnabled());
    assert(Debugger::IsDockUserEnabled(&dock));

    auto* toggle = dock.findChild<QPushButton*>();
    assert(toggle && toggle->isEnabled());
    assert(!toggle->isCheckable());
    toggle->click();
    assert(dock.isEnabled() && toggle->isEnabled() && !content->isEnabled() && !callback_active);
    assert(!Debugger::IsDockUserEnabled(&dock));

    Debugger::SetDockAvailable(&dock, true);
    toggle->click();
    assert(Debugger::IsDockActive(&dock) && content->isEnabled() && callback_active);

    dock.setFloating(true);
    app.processEvents();
    assert(dock.isEnabled());
    assert(dock.windowModality() == Qt::NonModal);
    return 0;
}
