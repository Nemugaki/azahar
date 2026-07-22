// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "debugger/render_session.h"

namespace Debugger {

bool SaveCapture(const std::string& path, const Capture& capture, std::string& error);
bool LoadCapture(const std::string& path, Capture& capture, std::string& error,
                 const CaptureLimits& limits = {});

} // namespace Debugger
