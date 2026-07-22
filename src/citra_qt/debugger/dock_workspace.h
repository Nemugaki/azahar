// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

class QDockWidget;

namespace Debugger {

/// Gives a debugger dock persistent workspace controls without replacing its existing content.
void ConfigureDockWorkspace(QDockWidget* dock);

/// Changes runtime availability while leaving the dock and its Enable/Disable control usable.
void SetDockAvailable(QDockWidget* dock, bool available);

/// True when both the user toggle and runtime availability permit the debugger to operate.
bool IsDockActive(const QDockWidget* dock);

} // namespace Debugger
