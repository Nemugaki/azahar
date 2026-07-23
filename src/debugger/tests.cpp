// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <cassert>
#include <filesystem>
#include <fstream>

#include "render_debugger/capture_file.h"

int main() {
    Debugger::Capture source{.producer = "test",
                             .backend = "software",
                             .timeline = {{1, Debugger::TimelineKind::Draw}}};
    std::vector<Debugger::u8> bytes;
    std::string error;
    assert(Debugger::SerializeCapture(source, bytes, error));

    Debugger::Capture loaded;
    assert(Debugger::DeserializeCapture(bytes, loaded, error));
    assert(loaded.producer == source.producer);
    assert(loaded.timeline.size() == 1);

    Debugger::CaptureReadLimits read_limits;
    read_limits.total_bytes = bytes.size() - 1;
    assert(!Debugger::DeserializeCapture(bytes, loaded, error, read_limits));

    Debugger::RenderSession session;
    Debugger::CaptureLimits live_limits;
    live_limits.timeline_entries = 1;
    session.SetCaptureLimits(live_limits);
    session.RecordDraw({});
    session.RecordDraw({});
    assert(session.GetStatus().count == 1);
    assert(session.GetStatus().truncated);

    const auto path = std::filesystem::temp_directory_path() / "render-debugger-atomic.rdbg";
    {
        std::ofstream existing{path};
        existing << "valid";
    }
    auto invalid = source;
    invalid.capabilities = 0;
    assert(!Debugger::SaveCapture(path.string(), invalid, error));
    std::ifstream preserved{path};
    std::string contents;
    preserved >> contents;
    assert(contents == "valid");
    std::filesystem::remove(path);
}
