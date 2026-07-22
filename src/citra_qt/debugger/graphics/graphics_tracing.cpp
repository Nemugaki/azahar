// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <QBoxLayout>
#include <QComboBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <nihstro/float24.h>
#include "citra_qt/debugger/graphics/graphics_tracing.h"
#include "common/common_types.h"
#include "core/core.h"
#include "core/tracer/recorder.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"

GraphicsTracingWidget::GraphicsTracingWidget(Core::System& system_,
                                             std::shared_ptr<Pica::DebugContext> debug_context,
                                             QWidget* parent)
    : BreakPointObserverDock(debug_context, tr("Pica Trace & Timeline"), parent), system{system_} {

    setObjectName(QStringLiteral("CiTracing"));

    QPushButton* start_recording = new QPushButton(tr("Start Recording"));
    QPushButton* stop_recording =
        new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("Stop and Save"));
    QPushButton* abort_recording = new QPushButton(tr("Abort Recording"));
    auto* refresh_timeline = new QPushButton(tr("Refresh Timeline"));
    timeline = new QTableWidget;
    timeline->setColumnCount(8);
    timeline->setHorizontalHeaderLabels(
        {tr("#"), tr("Kind"), tr("Frame"), tr("Draw"), tr("Changed"), tr("Color"),
         tr("Depth"), tr("Size")});
    timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);

    connect(this, &GraphicsTracingWidget::SetStartTracingButtonEnabled, start_recording,
            &QPushButton::setVisible);
    connect(this, &GraphicsTracingWidget::SetStopTracingButtonEnabled, stop_recording,
            &QPushButton::setVisible);
    connect(this, &GraphicsTracingWidget::SetAbortTracingButtonEnabled, abort_recording,
            &QPushButton::setVisible);
    connect(start_recording, &QPushButton::clicked, this, &GraphicsTracingWidget::StartRecording);
    connect(stop_recording, &QPushButton::clicked, this, &GraphicsTracingWidget::StopRecording);
    connect(abort_recording, &QPushButton::clicked, this, &GraphicsTracingWidget::AbortRecording);
    connect(refresh_timeline, &QPushButton::clicked, this, &GraphicsTracingWidget::RefreshTimeline);

    stop_recording->setVisible(false);
    abort_recording->setVisible(false);

    auto main_widget = new QWidget;
    auto main_layout = new QVBoxLayout;
    recording_controls = new QWidget;
    {
        auto sub_layout = new QHBoxLayout;
        sub_layout->addWidget(start_recording);
        sub_layout->addWidget(stop_recording);
        sub_layout->addWidget(abort_recording);
        recording_controls->setLayout(sub_layout);
        main_layout->addWidget(recording_controls);
    }
    main_layout->addWidget(refresh_timeline);
    main_layout->addWidget(timeline);
    main_widget->setLayout(main_layout);
    setWidget(main_widget);
}

void GraphicsTracingWidget::StartRecording() {
    auto context = context_weak.lock();
    if (!context)
        return;

    auto& pica = system.GPU().PicaCore();
    const auto& shader_binary = pica.vs_setup.GetProgramCode();
    const auto& swizzle_data = pica.vs_setup.GetSwizzleData();

    // Encode floating point numbers to 24-bit values
    // TODO: Drop this explicit conversion once we store float24 values bit-correctly internally.
    std::array<u32, 4 * 16> default_attributes{};
    for (u32 i = 0; i < 16; ++i) {
        for (u32 comp = 0; comp < 4; ++comp) {
            default_attributes[4 * i + comp] =
                nihstro::to_float24(pica.input_default_attributes[i][comp].ToFloat32());
        }
    }

    std::array<u32, 4 * 96> vs_float_uniforms{};
    for (u32 i = 0; i < 96; ++i) {
        for (u32 comp = 0; comp < 4; ++comp) {
            vs_float_uniforms[4 * i + comp] =
                nihstro::to_float24(pica.vs_setup.uniforms.f[i][comp].ToFloat32());
        }
    }

    CiTrace::Recorder::InitialState state;

    const auto copy = [&](std::vector<u32>& dest, const auto& data) {
        dest.resize(sizeof(data) / sizeof(dest.front()));
        std::memcpy(dest.data(), std::addressof(data), sizeof(data));
    };

    copy(state.pica_registers, pica.regs);
    copy(state.lcd_registers, pica.regs_lcd);
    copy(state.default_attributes, default_attributes);
    state.vs_program_binary.assign(shader_binary.begin(),
                                   shader_binary.begin() + pica.vs_setup.GetBiggestProgramSize());
    state.vs_swizzle_data.assign(swizzle_data.begin(),
                                 swizzle_data.begin() + pica.vs_setup.GetBiggestSwizzleSize());
    copy(state.vs_float_uniforms, vs_float_uniforms);
    // copy(TODO: Not implemented, std::back_inserter(state.gs_program_binary));
    // copy(TODO: Not implemented, std::back_inserter(state.gs_swizzle_data));
    // copy(TODO: Not implemented, std::back_inserter(state.gs_float_uniforms));

    context->recorder = std::make_shared<CiTrace::Recorder>(state);

    emit SetStartTracingButtonEnabled(false);
    emit SetStopTracingButtonEnabled(true);
    emit SetAbortTracingButtonEnabled(true);
}

void GraphicsTracingWidget::StopRecording() {
    auto context = context_weak.lock();
    if (!context)
        return;

    QString filename = QFileDialog::getSaveFileName(
        this, tr("Save CiTrace"), QStringLiteral("citrace.ctf"), tr("CiTrace File (*.ctf)"));

    if (filename.isEmpty()) {
        // If the user canceled the dialog, keep recording
        return;
    }

    context->recorder->Finish(filename.toStdString());
    context->recorder = nullptr;

    emit SetStopTracingButtonEnabled(false);
    emit SetAbortTracingButtonEnabled(false);
    emit SetStartTracingButtonEnabled(true);
}

void GraphicsTracingWidget::AbortRecording() {
    auto context = context_weak.lock();
    if (!context)
        return;

    context->recorder = nullptr;

    emit SetStopTracingButtonEnabled(false);
    emit SetAbortTracingButtonEnabled(false);
    emit SetStartTracingButtonEnabled(true);
}

void GraphicsTracingWidget::OnBreakPointHit(Pica::DebugContext::Event event, const void* data) {
    RefreshTimeline();
    recording_controls->setEnabled(true);
}

void GraphicsTracingWidget::RefreshTimeline() {
    auto context = context_weak.lock();
    if (!context) {
        return;
    }
    const auto entries = context->GetTimeline(UINT32_MAX, 128,
                                              Pica::DebugContext::TimelineKind::Draw, false);
    timeline->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
        const auto& entry = entries[row];
        const QString changed = QStringLiteral("%1%2%3%4")
                                    .arg(entry.changed_mask & 1 ? QStringLiteral("C") : QString{})
                                    .arg(entry.changed_mask & 2 ? QStringLiteral("D") : QString{})
                                    .arg(entry.changed_mask & 4 ? QStringLiteral("S") : QString{})
                                    .arg(entry.changed_mask & 8 ? QStringLiteral("F") : QString{});
        const QStringList values{
            QString::number(entry.sequence),
            entry.kind == Pica::DebugContext::TimelineKind::Draw ? tr("Draw") : tr("Frame"),
            QString::number(entry.frame),
            QString::number(entry.draw),
            changed,
            QStringLiteral("0x%1").arg(entry.target.color_address, 8, 16, QLatin1Char('0')),
            QStringLiteral("0x%1").arg(entry.target.depth_address, 8, 16, QLatin1Char('0')),
            QStringLiteral("%1x%2").arg(entry.target.width).arg(entry.target.height),
        };
        for (int column = 0; column < values.size(); ++column) {
            timeline->setItem(row, column, new QTableWidgetItem(values[column]));
        }
    }
    timeline->resizeColumnsToContents();
}

void GraphicsTracingWidget::OnResumed() {
    recording_controls->setEnabled(false);
}

void GraphicsTracingWidget::OnEmulationStarting(EmuThread* emu_thread) {
    // Disable tracing starting/stopping until a GPU breakpoint is reached
    recording_controls->setEnabled(false);
}

void GraphicsTracingWidget::OnEmulationStopping() {
    // TODO: Is it safe to access the context here?

    auto context = context_weak.lock();
    if (!context)
        return;

    if (context->recorder) {
        auto reply =
            QMessageBox::question(this, tr("CiTracing still active"),
                                  tr("A CiTrace is still being recorded. Do you want to save it? "
                                     "If not, all recorded data will be discarded."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (reply == QMessageBox::Yes) {
            StopRecording();
        } else {
            AbortRecording();
        }
    }

    recording_controls->setEnabled(true);
}
