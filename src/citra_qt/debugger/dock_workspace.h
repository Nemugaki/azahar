// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

#include <functional>

namespace ads {
class CDockWidget;
}

namespace Debugger {

/// Gives a debugger dock persistent workspace controls without replacing its existing content.
void ConfigureDockWorkspace(ads::CDockWidget* dock);

/// Changes runtime availability while leaving the dock and its Enable/Disable control usable.
void SetDockAvailable(ads::CDockWidget* dock, bool available);

/// True when both the user toggle and runtime availability permit the debugger to operate.
bool IsDockActive(const ads::CDockWidget* dock);

/// True when the user has enabled the debugger, regardless of runtime availability.
bool IsDockUserEnabled(const ads::CDockWidget* dock);

/// Runs when the workspace Enable/Disable state changes.
void SetDockActiveHandler(ads::CDockWidget* dock, std::function<void(bool)> handler);

} // namespace Debugger
