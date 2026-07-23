// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <chrono>
#include <catch2/catch_test_macros.hpp>
#include "core/core.h"
#include "video_core/debug_utils/debug_utils.h"

TEST_CASE("Render frame capture spans two presentation boundaries", "[video_core][debugger]") {
    using namespace std::chrono_literals;

    const auto context = Pica::DebugContext::Construct();
    const auto session = context->GetRenderSession();
    const Debugger::Shader shader{};
    const std::array<Debugger::u32, 0x300> registers{};
    session->RecordDraw({});

    REQUIRE(context->ArmFrameCapture());
    context->OnFramePresented();
    session->RecordDraw({}, shader, {}, registers);
    context->OnFramePresented();

    CHECK(context->WaitForFrameCapture(0ms));
    Debugger::Capture capture;
    session->Snapshot(capture);
    CHECK(capture.draws.size() == 1);
    CHECK(capture.timeline.size() == 3);

    context->FinishFrameCapture();
    Core::System::GetInstance().frame_limiter.SetFrameAdvancing(false);
}
