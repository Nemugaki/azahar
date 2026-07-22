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

    auto entries =
        session.Query({.start = 0, .count = 4, .required_changes = 3, .target_address = 0x1000});
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
    Debugger::Capture external{
        .producer = "external",
        .backend = "software",
        .capabilities = Debugger::Timeline,
        .complete = false,
        .truncated = status.truncated,
        .gap_reason = "missing previews",
        .timeline = session.Query({.start = Debugger::RenderSession::Latest, .count = 4}),
        .writes = {},
        .draws = {},
        .shaders = {},
        .resources = {},
    };
    const auto imported_id = sessions.AddImported(std::move(external));
    REQUIRE(imported_id != 0);
    CHECK(sessions.Get(imported_id));
    CHECK_FALSE(sessions.Get(9999));
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

TEST_CASE("Render debugger retains per-draw output while pruning old frames", "[debugger]") {
    Debugger::RenderSession session;
    const Debugger::Shader shader{.entry_point = 4, .code = {1}, .metadata = {}, .state = {}};
    const std::vector<Debugger::u8> first_pixels{1, 2, 3, 4};
    const std::vector<Debugger::u8> second_pixels{5, 6, 7, 8};
    session.SetFrameLimit(1);
    session.SetOutputCaptureEnabled(true);
    session.SetRenderTarget({0x1000, 0x2000, 1, 1, 0, 0});
    session.RecordDraw({Debugger::DrawMode::Arrays, 3, 0, 0, 4}, shader, {});
    session.RecordOutput(
        {Debugger::ResourceRole::ColorTarget, 0, 0x1000, 0, 1, 1, 4, first_pixels});
    const auto first_sequence = session.GetStatus().newest_sequence;
    REQUIRE(session.GetDrawOutput(first_sequence));
    CHECK(session.GetDrawOutput(first_sequence)->bytes == first_pixels);

    session.RecordFrame();
    session.RecordDraw({Debugger::DrawMode::Arrays, 6, 0, 0, 4}, shader, {});
    session.RecordOutput(
        {Debugger::ResourceRole::ColorTarget, 0, 0x1000, 0, 1, 1, 4, second_pixels});
    const auto second_sequence = session.GetStatus().newest_sequence;
    CHECK_FALSE(session.GetDrawOutput(first_sequence));
    REQUIRE(session.GetDrawOutput(second_sequence));
    CHECK(session.GetDrawOutput(second_sequence)->bytes == second_pixels);
    REQUIRE(session.GetDrawOutput(second_sequence, 1, 2));
    CHECK(session.GetDrawOutput(second_sequence, 1, 2)->bytes ==
          std::vector<Debugger::u8>{6, 7});
    CHECK_FALSE(session.GetDrawOutput(second_sequence, 5, 1));
}

TEST_CASE("Disabled render debugger does not capture", "[debugger]") {
    Debugger::RenderSession session;
    session.SetCaptureEnabled(false);
    session.RecordRegisterWrite({});
    session.RecordDraw({});
    session.RecordFrame();

    Debugger::Capture capture;
    session.Snapshot(capture);
    CHECK(session.GetStatus().count == 0);
    CHECK(capture.writes.empty());
    CHECK(capture.draws.empty());
}

TEST_CASE("Render debugger capture round-trips and rejects malformed input", "[debugger]") {
    const auto path = std::filesystem::temp_directory_path() / "azahar-render-debugger-test.rdbg";
    Debugger::Capture source{
        .producer = "test",
        .backend = "software",
        .capabilities =
            Debugger::Timeline | Debugger::RegisterWrites | Debugger::Resources | Debugger::Shaders,
        .gap_reason = {},
        .owned_bytes = 18,
        .timeline = {{7,
                      Debugger::TimelineKind::Draw,
                      2,
                      3,
                      1,
                      {0x1000, 0x2000, 400, 240, 1, 2},
                      {Debugger::DrawMode::Arrays, 6, 0, 4, 0x20}}},
        .writes = {{0, 0x200, 0x12345678, 0xF}},
        .draws = {},
        .shaders = {{1, 0, 0x20, {1, 2}, {3}, {4, 5}}},
        .resources = {{1, Debugger::ResourceRole::Texture, 0x3000, 0, 8, 8, 4, {6, 7, 8, 9}}},
    };
    source.draws = {{source.timeline.front(), 0, 1, 1, {{Debugger::ResourceRole::Texture, 0, 1}}}};
    std::string error;
    REQUIRE(Debugger::SaveCapture(path.string(), source, error));
    CHECK(std::filesystem::file_size(path) == 282);

    Debugger::Capture loaded;
    REQUIRE(Debugger::LoadCapture(path.string(), loaded, error));
    CHECK(loaded.producer == "test");
    REQUIRE(loaded.timeline.size() == 1);
    CHECK(loaded.timeline.front().target.color_address == 0x1000);
    REQUIRE(loaded.draws.size() == 1);
    CHECK(loaded.draws.front().write_count == 1);
    CHECK(loaded.shaders.front().code == std::vector<Debugger::u32>{1, 2});
    CHECK(loaded.resources.front().bytes == std::vector<Debugger::u8>{6, 7, 8, 9});
    Debugger::RenderSession imported;
    REQUIRE(imported.Replace(std::move(loaded)));
    CHECK(imported.GetPosition().draw == 3);
    REQUIRE(imported.GetDrawDetails(7));
    CHECK(imported.GetDrawDetails(7)->resource_count == 1);

    {
        std::ofstream trailing(path, std::ios::binary | std::ios::app);
        trailing.put('\0');
    }
    CHECK_FALSE(Debugger::LoadCapture(path.string(), loaded, error));

    {
        std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
        malformed << "not a capture";
    }
    CHECK_FALSE(Debugger::LoadCapture(path.string(), loaded, error));
    CHECK_FALSE(error.empty());
    std::filesystem::remove(path);
}
