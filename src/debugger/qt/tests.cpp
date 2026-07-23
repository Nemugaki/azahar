// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include <array>
#include <cassert>

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QSlider>
#include <QTreeWidget>

#include "capture_viewer.h"

int main(int argc, char** argv) {
    QApplication application{argc, argv};
    auto sessions = std::make_shared<Debugger::RenderSessionManager>("test", "software");
    auto live = sessions->GetLive();
    live->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::UserInterface, true);
    live->SetRenderTarget({0x1000, 0x2000, 64, 32, 0, 1});
    Debugger::Shader shader{.stage = 0, .entry_point = 4, .code = {1, 2, 3}};
    std::array<Debugger::ResourceView, 0> resources;
    std::array<Debugger::u32, 0x300> registers{};
    registers[4] = 1;
    const Debugger::DrawInfo draw{Debugger::DrawMode::Indexed, 24, 0, 8, 4};
    std::array<Debugger::u8, 8 * 8 * 4> pixels{};
    live->RecordDraw(draw, shader, resources, registers);
    live->RecordOutput({Debugger::ResourceRole::ColorTarget, 0, 0x1000, 0, 8, 8, 32,
                        Debugger::ResourceTiling::PicaTiled, Debugger::ResourceOrigin::BottomLeft,
                        pixels});
    live->RecordDraw(draw, shader, resources, registers);
    live->RecordOutput({Debugger::ResourceRole::ColorTarget, 0, 0x2000, 0, 8, 8, 32,
                        Debugger::ResourceTiling::PicaTiled, Debugger::ResourceOrigin::BottomLeft,
                        pixels});
    live->RecordFrame();
    live->SetRenderTarget({0x3000, 0x4000, 32, 32, 2, 3});
    live->RecordDraw(draw, shader, resources, registers);

    Debugger::CaptureViewerWidget viewer{sessions};
    viewer.Refresh();

    auto* timeline = viewer.findChild<QTreeWidget*>(QStringLiteral("renderTimeline"));
    auto* slider = viewer.findChild<QSlider*>(QStringLiteral("renderEventSlider"));
    auto* details = viewer.findChild<QLabel*>(QStringLiteral("renderEventDetails"));
    auto* output = viewer.findChild<QLabel*>(QStringLiteral("renderOutputStatus"));
    assert(timeline && timeline->topLevelItemCount() == 2);
    assert(timeline->topLevelItem(0)->child(0)->child(0)->childCount() == 2);
    assert(slider && slider->maximum() == 2);
    slider->setValue(0);
    assert(output && output->text().contains(QStringLiteral("0x1000")));
    slider->setValue(1);
    assert(output->text().contains(QStringLiteral("0x2000")));
    assert(viewer.SelectedTarget().has_value());
    assert(details && details->text().contains(QStringLiteral("offset 8")));
    assert(details->text().contains(QStringLiteral("Immutable render state")));

    viewer.findChild<QCheckBox*>(QStringLiteral("followRenderLive"))->setChecked(false);
    live->SetFrameLimit(1);
    live->RecordFrame();
    live->SetRenderTarget({0x5000, 0x6000, 16, 16, 0, 1});
    live->RecordDraw(draw, shader, resources, registers);
    viewer.Refresh();
    assert(timeline->currentItem());
    assert(viewer.SelectedTarget()->color_address == 0x5000);
    return 0;
}
