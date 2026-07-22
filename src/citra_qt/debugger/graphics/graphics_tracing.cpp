// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <QBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTimer>
#include <nihstro/float24.h>
#include "citra_qt/debugger/graphics/graphics_surface.h"
#include "citra_qt/debugger/graphics/graphics_tracing.h"
#include "citra_qt/debugger/dock_workspace.h"
#include "common/common_types.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/tracer/recorder.h"
#include "debugger/capture_file.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"

GraphicsTracingWidget::GraphicsTracingWidget(Core::System& system_,
                                             std::shared_ptr<Pica::DebugContext> debug_context,
                                             QWidget* parent)
    : BreakPointObserverDock(debug_context, tr("Pica Trace & Timeline"), parent), system{system_},
      render_sessions{debug_context ? debug_context->GetRenderSessions() : nullptr} {

    setObjectName(QStringLiteral("CiTracing"));

    QPushButton* start_recording = new QPushButton(tr("Start Recording"));
    QPushButton* stop_recording =
        new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("Stop and Save"));
    QPushButton* abort_recording = new QPushButton(tr("Abort Recording"));
    auto* refresh_timeline = new QPushButton(tr("Refresh Timeline"));
    timeline_filter = new QLineEdit;
    timeline_filter->setPlaceholderText(tr("Filter draws, frames, targets, or topology"));
    timeline_filter->setClearButtonEnabled(true);
    timeline_filter->setAccessibleName(tr("Render timeline filter"));
    frame_limit = new QSpinBox;
    frame_limit->setRange(1, 120);
    frame_limit->setValue(Settings::values.render_debugger_frame_limit.GetValue());
    frame_limit->setSuffix(tr(" frames"));
    frame_limit->setAccessibleName(tr("Maximum retained render timeline frames"));
    frame_limit->setToolTip(
        tr("Keeps metadata for this many recent frames, up to 4096 events. Oldest entries are "
           "evicted; render-target images are not copied."));
    follow_live = new QCheckBox(tr("Follow Live"));
    follow_live->setChecked(true);
    freeze_timeline = new QCheckBox(tr("Freeze"));
    freeze_timeline->setToolTip(tr("Stops automatic refresh without pausing emulation."));
    if (render_sessions) {
        render_sessions->GetLive()->SetFrameLimit(frame_limit->value());
    }
    timeline = new QTreeWidget;
    timeline->setColumnCount(6);
    timeline->setHeaderLabels(
        {tr("Call"), tr("Category"), tr("Topology"), tr("Vertices"), tr("Target"),
         tr("Changed")});
    timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline->setAlternatingRowColors(true);
    timeline->setAccessibleName(tr("Ordered Pica draw-call timeline"));
    timeline->header()->setStretchLastSection(false);
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    timeline_details = new QLabel(tr("Select a draw call to inspect it."));
    timeline_details->setWordWrap(true);
    timeline_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    open_color_target = new QPushButton(tr("Open Color Target"));
    open_depth_target = new QPushButton(tr("Open Depth Target"));
    const QString target_help =
        tr("Opens this historical target's address and format using its current memory contents.");
    open_color_target->setToolTip(target_help);
    open_depth_target->setToolTip(target_help);
    open_color_target->setEnabled(false);
    open_depth_target->setEnabled(false);

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
    auto* refresh_timer = new QTimer(this);
    refresh_timer->setInterval(500);
    connect(refresh_timer, &QTimer::timeout, this, [this] {
        if (follow_live->isChecked() && !freeze_timeline->isChecked()) {
            RefreshTimeline();
        }
    });
    refresh_timer->start();
    connect(timeline_filter, &QLineEdit::textChanged, this,
            &GraphicsTracingWidget::FilterTimeline);
    connect(frame_limit, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        Settings::values.render_debugger_frame_limit = static_cast<u32>(value);
        if (render_sessions) {
            render_sessions->GetLive()->SetFrameLimit(static_cast<u32>(value));
        }
        RefreshTimeline();
    });
    connect(timeline, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { SelectTimelineEntry(current); });
    connect(timeline, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item) {
                SelectTimelineEntry(item);
                OpenColorTarget();
            });
    connect(open_color_target, &QPushButton::clicked, this,
            &GraphicsTracingWidget::OpenColorTarget);
    connect(open_depth_target, &QPushButton::clicked, this,
            &GraphicsTracingWidget::OpenDepthTarget);

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
    auto* timeline_controls = new QHBoxLayout;
    auto* filter_label = new QLabel(tr("Filter:"));
    filter_label->setBuddy(timeline_filter);
    timeline_controls->addWidget(filter_label);
    timeline_controls->addWidget(timeline_filter);
    auto* limit_label = new QLabel(tr("Keep:"));
    limit_label->setBuddy(frame_limit);
    timeline_controls->addWidget(limit_label);
    timeline_controls->addWidget(frame_limit);
    timeline_controls->addWidget(follow_live);
    timeline_controls->addWidget(freeze_timeline);
    timeline_controls->addWidget(refresh_timeline);
    main_layout->addLayout(timeline_controls);
    auto* session_controls = new QHBoxLayout;
    auto* session_label = new QLabel(tr("Session:"));
    session_selector = new QComboBox;
    session_selector->setAccessibleName(tr("Render debugger session"));
    session_label->setBuddy(session_selector);
    auto* import_capture = new QPushButton(tr("Import Capture"));
    auto* export_capture = new QPushButton(tr("Export Capture"));
    remove_capture = new QPushButton(tr("Remove Capture"));
    connect(session_selector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 0 && render_sessions) {
                    render_sessions->Select(
                        session_selector->itemData(index).toULongLong());
                    remove_capture->setEnabled(render_sessions->GetActiveId() !=
                                               Debugger::RenderSessionManager::LiveSessionId);
                    RefreshTimeline();
                }
            });
    connect(import_capture, &QPushButton::clicked, this, &GraphicsTracingWidget::ImportCapture);
    connect(export_capture, &QPushButton::clicked, this, &GraphicsTracingWidget::ExportCapture);
    connect(remove_capture, &QPushButton::clicked, this, &GraphicsTracingWidget::RemoveCapture);
    session_controls->addWidget(session_label);
    session_controls->addWidget(session_selector);
    session_controls->addWidget(import_capture);
    session_controls->addWidget(export_capture);
    session_controls->addWidget(remove_capture);
    main_layout->addLayout(session_controls);
    main_layout->addWidget(timeline);
    main_layout->addWidget(timeline_details);
    auto* target_controls = new QHBoxLayout;
    auto* previous_event = new QPushButton(tr("Previous Event"));
    auto* next_event = new QPushButton(tr("Next Event"));
    connect(previous_event, &QPushButton::clicked, this, [this] {
        if (auto* item = timeline->itemAbove(timeline->currentItem())) {
            timeline->setCurrentItem(item);
        }
    });
    connect(next_event, &QPushButton::clicked, this, [this] {
        if (auto* item = timeline->itemBelow(timeline->currentItem())) {
            timeline->setCurrentItem(item);
        }
    });
    target_controls->addWidget(previous_event);
    target_controls->addWidget(next_event);
    target_controls->addWidget(open_color_target);
    target_controls->addWidget(open_depth_target);
    main_layout->addLayout(target_controls);
    main_widget->setLayout(main_layout);
    setWidget(main_widget);
    RefreshSessions();
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

    if (!context->recorder->Finish(filename.toStdString())) {
        QMessageBox::critical(
            this, tr("CiTrace capture incomplete"),
            tr("The capture exceeded the configured debugger cache limit and was not saved."));
    }
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
    if (!render_sessions) {
        return;
    }
    const auto session_id = render_sessions->GetActiveId();
    const auto session = render_sessions->GetActive();
    const auto status = session->GetStatus();
    if (have_displayed_status && displayed_session_id == session_id &&
        displayed_status.count == status.count &&
        displayed_status.oldest_sequence == status.oldest_sequence &&
        displayed_status.newest_sequence == status.newest_sequence &&
        displayed_status.truncated == status.truncated) {
        return;
    }

    const auto* current = timeline->currentItem();
    std::optional<u32> selected_sequence;
    if (current && current->parent()) {
        const int index = current->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(displayed_entries.size())) {
            selected_sequence = displayed_entries[index].sequence;
        }
    }
    auto* scroll_bar = timeline->verticalScrollBar();
    const int old_scroll = scroll_bar->value();
    const bool was_at_bottom = old_scroll >= scroll_bar->maximum();

    displayed_entries = session->Query(
        {.start = Debugger::RenderSession::Latest, .count = 4096});
    displayed_session_id = session_id;
    displayed_status = session->GetStatus();
    have_displayed_status = true;
    timeline->clear();
    QTreeWidgetItem* frame_item = nullptr;
    u32 displayed_frame = UINT32_MAX;
    const QStringList topologies{tr("Triangles"), tr("Triangle strip"), tr("Triangle fan"),
                                 tr("Geometry shader")};
    for (int index = 0; index < static_cast<int>(displayed_entries.size()); ++index) {
        const auto& entry = displayed_entries[index];
        if (!frame_item || displayed_frame != entry.frame) {
            displayed_frame = entry.frame;
            frame_item = new QTreeWidgetItem(timeline, {tr("Frame %1").arg(entry.frame)});
            frame_item->setFirstColumnSpanned(true);
            frame_item->setExpanded(true);
        }
        if (entry.kind == Debugger::TimelineKind::Frame) {
            continue;
        }
        QStringList changes;
        if (entry.changed_mask & 1)
            changes << tr("Color");
        if (entry.changed_mask & 2)
            changes << tr("Depth");
        if (entry.changed_mask & 4)
            changes << tr("Size");
        if (entry.changed_mask & 8)
            changes << tr("Format");
        const QString changed = changes.join(QStringLiteral(", "));
        const QString call = [&] {
            switch (entry.draw_info.mode) {
            case Debugger::DrawMode::Indexed:
                return tr("Draw %1 — indexed").arg(entry.draw);
            case Debugger::DrawMode::Immediate:
                return tr("Draw %1 — immediate").arg(entry.draw);
            default:
                return tr("Draw %1 — arrays").arg(entry.draw);
            }
        }();
        const QString category = entry.changed_mask & 3 ? tr("Render target") : tr("Geometry");
        const QString topology = entry.draw_info.topology < topologies.size()
                                     ? topologies[entry.draw_info.topology]
                                     : tr("Unknown");
        auto* item = new QTreeWidgetItem(
            frame_item,
            {call, category, topology, QString::number(entry.draw_info.vertex_count),
             QStringLiteral("0x%1 · %2x%3")
                 .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                 .arg(entry.target.width)
                 .arg(entry.target.height),
             changed});
        item->setData(0, Qt::UserRole, index);
    }
    for (int column = 1; column < timeline->columnCount(); ++column) {
        timeline->resizeColumnToContents(column);
    }
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    FilterTimeline(timeline_filter->text());
    if (follow_live->isChecked() && was_at_bottom && timeline->topLevelItemCount()) {
        auto* frame = timeline->topLevelItem(timeline->topLevelItemCount() - 1);
        if (frame->childCount()) {
            timeline->setCurrentItem(frame->child(frame->childCount() - 1));
            timeline->scrollToItem(timeline->currentItem());
        }
    } else {
        if (selected_sequence) {
            for (int frame = 0; frame < timeline->topLevelItemCount(); ++frame) {
                auto* frame_item = timeline->topLevelItem(frame);
                for (int draw = 0; draw < frame_item->childCount(); ++draw) {
                    auto* item = frame_item->child(draw);
                    const int index = item->data(0, Qt::UserRole).toInt();
                    if (index >= 0 && index < static_cast<int>(displayed_entries.size()) &&
                        displayed_entries[index].sequence == *selected_sequence) {
                        timeline->setCurrentItem(item);
                        frame = timeline->topLevelItemCount();
                        break;
                    }
                }
            }
        }
        scroll_bar->setValue(old_scroll);
    }
}

void GraphicsTracingWidget::FilterTimeline(const QString& text) {
    const QString needle = text.trimmed();
    for (int frame = 0; frame < timeline->topLevelItemCount(); ++frame) {
        auto* frame_item = timeline->topLevelItem(frame);
        bool frame_matches = frame_item->text(0).contains(needle, Qt::CaseInsensitive);
        bool any_visible = false;
        for (int draw = 0; draw < frame_item->childCount(); ++draw) {
            auto* item = frame_item->child(draw);
            bool matches = needle.isEmpty() || frame_matches;
            for (int column = 0; !matches && column < timeline->columnCount(); ++column) {
                matches = item->text(column).contains(needle, Qt::CaseInsensitive);
            }
            item->setHidden(!matches);
            any_visible |= matches;
        }
        frame_item->setHidden(!any_visible);
    }
}

void GraphicsTracingWidget::SelectTimelineEntry(QTreeWidgetItem* current) {
    selected_target.reset();
    if (!current || !current->parent()) {
        timeline_details->setText(tr("Select a draw call to inspect it."));
        open_color_target->setEnabled(false);
        open_depth_target->setEnabled(false);
        return;
    }
    const int index = current->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(displayed_entries.size())) {
        return;
    }
    const auto& entry = displayed_entries[index];
    selected_target = entry.target;
    timeline_details->setText(
        tr("Sequence %1 · frame %2 · draw %3 · vertex offset %4 · VS entry 0x%5\n"
           "Color 0x%6 · depth 0x%7 · %8x%9")
            .arg(entry.sequence)
            .arg(entry.frame)
            .arg(entry.draw)
            .arg(entry.draw_info.vertex_offset)
            .arg(entry.draw_info.vertex_shader_entry, 0, 16)
            .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
            .arg(entry.target.depth_address, 8, 16, QLatin1Char('0'))
            .arg(entry.target.width)
            .arg(entry.target.height));
    const bool live = render_sessions &&
                      render_sessions->GetActiveId() ==
                          Debugger::RenderSessionManager::LiveSessionId;
    open_color_target->setEnabled(live && entry.target.color_address != 0);
    open_depth_target->setEnabled(live && entry.target.depth_address != 0);
    if (!live) {
        timeline_details->setText(timeline_details->text() +
                                  tr("\nImported metadata has no render-target bytes."));
    }
}

void GraphicsTracingWidget::OpenColorTarget() {
    OpenTarget(false);
}

void GraphicsTracingWidget::OpenDepthTarget() {
    OpenTarget(true);
}

void GraphicsTracingWidget::OpenTarget(bool depth) {
    if (!selected_target || !render_sessions ||
        render_sessions->GetActiveId() != Debugger::RenderSessionManager::LiveSessionId) {
        return;
    }
    auto* viewer = new GraphicsSurfaceWidget(system, context_weak.lock(), parentWidget());
    Debugger::ConfigureDockWorkspace(viewer);
    if (auto* main = qobject_cast<QMainWindow*>(parentWidget())) {
        main->addDockWidget(Qt::RightDockWidgetArea, viewer);
    }
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setFloating(true);
    viewer->ViewRenderTarget(*selected_target, depth);
    viewer->resize(640, 520);
    viewer->show();
}

void GraphicsTracingWidget::ImportCapture() {
    if (!render_sessions) {
        return;
    }
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Import Render Debugger Capture"), {}, tr("Render Debugger Capture (*.rdbg)"));
    if (filename.isEmpty()) {
        return;
    }
    Debugger::Capture capture;
    Debugger::CaptureLimits limits;
    limits.total_bytes = static_cast<u64>(Settings::values.debugger_cache_mb.GetValue()) << 20;
    std::string error;
    if (!Debugger::LoadCapture(filename.toStdString(), capture, error, limits)) {
        QMessageBox::critical(this, tr("Capture import failed"), QString::fromStdString(error));
        return;
    }
    const auto id = render_sessions->AddImported(std::move(capture));
    if (!id) {
        QMessageBox::critical(this, tr("Capture import failed"),
                              tr("The capture is invalid or the imported-session limit was reached."));
        return;
    }
    RefreshSessions(id);
    RefreshTimeline();
}

void GraphicsTracingWidget::ExportCapture() {
    if (!render_sessions) {
        return;
    }
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("Export Render Debugger Capture"), QStringLiteral("render-capture.rdbg"),
        tr("Render Debugger Capture (*.rdbg)"));
    if (filename.isEmpty()) {
        return;
    }
    Debugger::Capture capture;
    if (!render_sessions->Snapshot(render_sessions->GetActiveId(), capture)) {
        return;
    }
    std::string error;
    if (!Debugger::SaveCapture(filename.toStdString(), capture, error)) {
        QMessageBox::critical(this, tr("Capture export failed"), QString::fromStdString(error));
    }
}

void GraphicsTracingWidget::RemoveCapture() {
    if (!render_sessions) {
        return;
    }
    render_sessions->Remove(render_sessions->GetActiveId());
    RefreshSessions(Debugger::RenderSessionManager::LiveSessionId);
    RefreshTimeline();
}

void GraphicsTracingWidget::RefreshSessions(Debugger::u64 selected) {
    if (!render_sessions) {
        return;
    }
    if (!selected) {
        selected = render_sessions->GetActiveId();
    }
    const QSignalBlocker blocker{session_selector};
    session_selector->clear();
    int selected_index = 0;
    for (const auto& descriptor : render_sessions->List()) {
        const QString name = descriptor.live
                                 ? tr("Live — %1/%2")
                                       .arg(QString::fromStdString(descriptor.producer),
                                            QString::fromStdString(descriptor.backend))
                                 : tr("Imported — %1/%2")
                                       .arg(QString::fromStdString(descriptor.producer),
                                            QString::fromStdString(descriptor.backend));
        session_selector->addItem(name, QVariant::fromValue<qulonglong>(descriptor.id));
        if (descriptor.id == selected) {
            selected_index = session_selector->count() - 1;
        }
    }
    session_selector->setCurrentIndex(selected_index);
    render_sessions->Select(selected);
    remove_capture->setEnabled(selected != Debugger::RenderSessionManager::LiveSessionId);
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
