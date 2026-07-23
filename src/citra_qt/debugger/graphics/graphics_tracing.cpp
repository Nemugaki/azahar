// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include "citra_qt/debugger/graphics/graphics_tracing.h"

#include <array>
#include <cstring>

#include <DockManager.h>
#include <QBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>

#include <nihstro/float24.h>

#include "capture_viewer.h"
#include "citra_qt/debugger/dock_workspace.h"
#include "citra_qt/debugger/graphics/graphics_surface.h"
#include "core/core.h"
#include "core/tracer/recorder.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"

GraphicsTracingWidget::GraphicsTracingWidget(
    Core::System& system_, std::shared_ptr<Pica::DebugContext> debug_context, QWidget* parent)
    : BreakPointObserverDock(debug_context, tr("Pica Trace & Timeline"), parent), system{system_} {
    setObjectName(QStringLiteral("CiTracing"));

    auto* start = new QPushButton{tr("Start Recording")};
    auto* stop = new QPushButton{tr("Stop and Save")};
    auto* abort = new QPushButton{tr("Abort Recording")};
    stop->setVisible(false);
    abort->setVisible(false);
    connect(this, &GraphicsTracingWidget::SetStartTracingButtonEnabled, start,
            &QPushButton::setVisible);
    connect(this, &GraphicsTracingWidget::SetStopTracingButtonEnabled, stop,
            &QPushButton::setVisible);
    connect(this, &GraphicsTracingWidget::SetAbortTracingButtonEnabled, abort,
            &QPushButton::setVisible);
    connect(start, &QPushButton::clicked, this, &GraphicsTracingWidget::StartRecording);
    connect(stop, &QPushButton::clicked, this, &GraphicsTracingWidget::StopRecording);
    connect(abort, &QPushButton::clicked, this, &GraphicsTracingWidget::AbortRecording);

    auto* layout = new QVBoxLayout;
    recording_controls = new QWidget;
    auto* recording_layout = new QHBoxLayout{recording_controls};
    recording_layout->addWidget(start);
    recording_layout->addWidget(stop);
    recording_layout->addWidget(abort);
    layout->addWidget(recording_controls);

    capture_viewer = new Debugger::CaptureViewerWidget{
        debug_context ? debug_context->GetRenderSessions() : nullptr};
    connect(capture_viewer, &Debugger::CaptureViewerWidget::OpenTargetRequested, this,
            &GraphicsTracingWidget::OpenTarget);
    layout->addWidget(capture_viewer, 1);
    auto* content = new QWidget;
    content->setLayout(layout);
    setWidget(content);

    Debugger::SetDockActiveHandler(this,
                                   [this](bool active) { capture_viewer->SetActive(active); });
    connect(this, &ads::CDockWidget::visibilityChanged, this,
            [this](bool visible) {
                capture_viewer->SetActive(visible && Debugger::IsDockActive(this));
            });
}

void GraphicsTracingWidget::StartRecording() {
    auto context = context_weak.lock();
    if (!context) {
        return;
    }
    auto& pica = system.GPU().PicaCore();
    const auto& shader_binary = pica.vs_setup.GetProgramCode();
    const auto& swizzle_data = pica.vs_setup.GetSwizzleData();
    std::array<u32, 4 * 16> default_attributes{};
    for (u32 index = 0; index < 16; ++index) {
        for (u32 component = 0; component < 4; ++component) {
            default_attributes[4 * index + component] =
                nihstro::to_float24(pica.input_default_attributes[index][component].ToFloat32());
        }
    }
    std::array<u32, 4 * 96> vs_float_uniforms{};
    for (u32 index = 0; index < 96; ++index) {
        for (u32 component = 0; component < 4; ++component) {
            vs_float_uniforms[4 * index + component] =
                nihstro::to_float24(pica.vs_setup.uniforms.f[index][component].ToFloat32());
        }
    }
    CiTrace::Recorder::InitialState state;
    const auto copy = [](std::vector<u32>& destination, const auto& source) {
        destination.resize(sizeof(source) / sizeof(destination.front()));
        std::memcpy(destination.data(), std::addressof(source), sizeof(source));
    };
    copy(state.pica_registers, pica.regs);
    copy(state.lcd_registers, pica.regs_lcd);
    copy(state.default_attributes, default_attributes);
    state.vs_program_binary.assign(shader_binary.begin(),
                                   shader_binary.begin() + pica.vs_setup.GetBiggestProgramSize());
    state.vs_swizzle_data.assign(swizzle_data.begin(),
                                 swizzle_data.begin() + pica.vs_setup.GetBiggestSwizzleSize());
    copy(state.vs_float_uniforms, vs_float_uniforms);
    context->recorder = std::make_shared<CiTrace::Recorder>(state);
    emit SetStartTracingButtonEnabled(false);
    emit SetStopTracingButtonEnabled(true);
    emit SetAbortTracingButtonEnabled(true);
}

void GraphicsTracingWidget::StopRecording() {
    auto context = context_weak.lock();
    if (!context) {
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("Save CiTrace"), QStringLiteral("citrace.ctf"), tr("CiTrace File (*.ctf)"));
    if (filename.isEmpty()) {
        return;
    }
    if (!context->recorder->Finish(filename.toStdString())) {
        QMessageBox::critical(
            this, tr("CiTrace capture incomplete"),
            tr("The capture exceeded the configured debugger cache limit and was not saved."));
    }
    context->recorder.reset();
    emit SetStopTracingButtonEnabled(false);
    emit SetAbortTracingButtonEnabled(false);
    emit SetStartTracingButtonEnabled(true);
}

void GraphicsTracingWidget::AbortRecording() {
    if (auto context = context_weak.lock()) {
        context->recorder.reset();
    }
    emit SetStopTracingButtonEnabled(false);
    emit SetAbortTracingButtonEnabled(false);
    emit SetStartTracingButtonEnabled(true);
}

void GraphicsTracingWidget::OpenTarget(bool depth) {
    const auto target = capture_viewer->SelectedTarget();
    if (!target) {
        return;
    }
    auto* viewer = new GraphicsSurfaceWidget(system, context_weak.lock(), dockManager());
    Debugger::ConfigureDockWorkspace(viewer);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    dockManager()->addDockWidgetFloating(viewer);
    viewer->ViewRenderTarget(*target, depth);
    viewer->resize(640, 520);
    viewer->toggleView(true);
}

void GraphicsTracingWidget::OnBreakPointHit(Pica::DebugContext::Event, const void*) {
    if (Debugger::IsDockActive(this)) {
        capture_viewer->Refresh();
        recording_controls->setEnabled(true);
    }
}

void GraphicsTracingWidget::OnResumed() {
    recording_controls->setEnabled(false);
}

void GraphicsTracingWidget::OnEmulationStarting(EmuThread*) {
    recording_controls->setEnabled(false);
}

void GraphicsTracingWidget::OnEmulationStopping() {
    auto context = context_weak.lock();
    if (!context || !context->recorder) {
        return;
    }
    const auto reply = QMessageBox::question(
        this, tr("CiTracing still active"),
        tr("A CiTrace is still being recorded. Do you want to save it? If not, all recorded data "
           "will be discarded."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply == QMessageBox::Yes) {
        StopRecording();
    } else {
        AbortRecording();
    }
}
