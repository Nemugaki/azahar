// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "debugger/capture_file.h"
#include "debugger/render_session.h"

TEST_CASE("Render debugger session retains and filters bounded metadata", "[debugger]") {
    Debugger::RenderSession session;
    session.SetRenderTarget({0x1000, 0x2000, 400, 240, 1, 2});
    session.RecordDraw({Debugger::DrawMode::Indexed, 12, 1, 4, 0x20});

    auto entries = session.Query(
        {.start = 0, .count = 4, .required_changes = 3, .target_address = 0x1000});
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().draw_info.mode == Debugger::DrawMode::Indexed);
    CHECK(entries.front().changed_mask == 15);

    CHECK(session.Query({.start = 0, .count = 0}).empty());
    session.SetFrameLimit(1);
    session.RecordFrame();
    const auto status = session.GetStatus();
    CHECK(status.count == 1);
    CHECK(status.truncated);
    CHECK(session.GetPosition().frame == 1);

    Debugger::RenderSessionManager sessions;
    const auto imported_id = sessions.AddImported(
        {"external", "software", Debugger::Timeline, false, status.truncated, "missing previews",
         session.Query({.start = Debugger::RenderSession::Latest, .count = 4})});
    REQUIRE(imported_id != 0);
    CHECK(sessions.GetActiveId() == imported_id);
    CHECK(sessions.GetActive()->GetStatus().count == 1);
    Debugger::Capture snapshot;
    REQUIRE(sessions.Snapshot(imported_id, snapshot));
    CHECK(snapshot.gap_reason == "missing previews");
    CHECK_FALSE(snapshot.complete);
    REQUIRE(sessions.Remove(imported_id));
    CHECK(sessions.GetActiveId() == Debugger::RenderSessionManager::LiveSessionId);

    session.Clear();
    CHECK(session.GetStatus().count == 0);
    CHECK_FALSE(session.GetStatus().truncated);
}

TEST_CASE("Render debugger capture round-trips and rejects malformed input", "[debugger]") {
    const auto path = std::filesystem::temp_directory_path() / "azahar-render-debugger-test.rdbg";
    Debugger::Capture source{
        .producer = "test",
        .backend = "software",
        .timeline = {{7,
                      Debugger::TimelineKind::Draw,
                      2,
                      3,
                      1,
                      {0x1000, 0x2000, 400, 240, 1, 2},
                      {Debugger::DrawMode::Arrays, 6, 0, 4, 0x20}}},
    };
    std::string error;
    REQUIRE(Debugger::SaveCapture(path.string(), source, error));

    Debugger::Capture loaded;
    REQUIRE(Debugger::LoadCapture(path.string(), loaded, error));
    CHECK(loaded.producer == "test");
    REQUIRE(loaded.timeline.size() == 1);
    CHECK(loaded.timeline.front().target.color_address == 0x1000);
    Debugger::RenderSession imported;
    REQUIRE(imported.Replace(loaded.timeline, loaded.truncated));
    CHECK(imported.GetPosition().draw == 3);

    {
        std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
        malformed << "not a capture";
    }
    CHECK_FALSE(Debugger::LoadCapture(path.string(), loaded, error));
    CHECK_FALSE(error.empty());
    std::filesystem::remove(path);
}
