// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "render_debugger/render_session.h"

namespace Debugger {

struct CaptureReadLimits {
    u64 total_bytes{1ULL << 30};
    CaptureLimits capture{.timeline_entries = 1'000'000};
};

bool SerializeCapture(const Capture& capture, std::vector<u8>& bytes, std::string& error);
bool DeserializeCapture(std::span<const u8> bytes, Capture& capture, std::string& error,
                        const CaptureReadLimits& limits = {});
bool SaveCapture(const std::string& path, const Capture& capture, std::string& error);
bool LoadCapture(const std::string& path, Capture& capture, std::string& error,
                 const CaptureReadLimits& limits = {});

} // namespace Debugger
