// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <DockManager.h>
#include <nihstro/float24.h>
#include "citra_qt/debugger/dock_workspace.h"
#include "citra_qt/debugger/graphics/graphics_surface.h"
#include "citra_qt/debugger/graphics/graphics_tracing.h"
#include "common/common_types.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/tracer/recorder.h"
#include "debugger/capture_file.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/texture/texture_decode.h"

class ScaledPixmapLabel final : public QLabel {
public:
    using QLabel::QLabel;

    void SetPixmap(QPixmap pixmap) {
        source = std::move(pixmap);
        UpdatePixmap();
    }

    QSize sizeHint() const override {
        return {};
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        UpdatePixmap();
    }

private:
    void UpdatePixmap() {
        QLabel::setPixmap(source.isNull()
                              ? source
                              : source.scaled(contentsRect().size(), Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
    }

    QPixmap source;
};

namespace {

QImage DecodeColorTarget(const Debugger::Resource& resource) {
    if (resource.width == 0 || resource.height == 0 || resource.bytes.empty() ||
        resource.format > static_cast<u32>(Pica::TexturingRegs::TextureFormat::RGBA4)) {
        return {};
    }

    Pica::Texture::TextureInfo info{};
    info.width = resource.width;
    info.height = resource.height;
    info.format = static_cast<Pica::TexturingRegs::TextureFormat>(resource.format);
    info.SetDefaultStride();
    const auto rows = (resource.height + 7) / 8;
    if (info.stride <= 0 || static_cast<u64>(info.stride) * rows > resource.bytes.size()) {
        return {};
    }

    QImage image(resource.width, resource.height, QImage::Format_ARGB32);
    for (u32 y = 0; y < resource.height; ++y) {
        for (u32 x = 0; x < resource.width; ++x) {
            const auto color = Pica::Texture::LookupTexture(resource.bytes.data(), x, y, info);
            image.setPixel(x, y, qRgba(color.r(), color.g(), color.b(), color.a()));
        }
    }
    return image;
}

} // namespace

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
           "evicted; rich draw data is bounded by the debugger cache limit."));
    follow_live = new QCheckBox(tr("Follow Live"));
    follow_live->setChecked(true);
    freeze_timeline = new QCheckBox(tr("Freeze"));
    freeze_timeline->setToolTip(tr("Stops automatic refresh without pausing emulation."));
    if (render_sessions) {
        render_sessions->GetLive()->SetFrameLimit(frame_limit->value());
        Debugger::SetDockActiveHandler(this, [this](bool active) {
            render_sessions->GetLive()->SetCaptureEnabled(active);
            UpdateOutputCaptureState();
        });
        connect(this, &ads::CDockWidget::visibilityChanged, this,
                [this](bool) { UpdateOutputCaptureState(); });
    }
    timeline = new QTreeWidget;
    timeline->setColumnCount(6);
    timeline->setHeaderLabels(
        {tr("Call"), tr("Category"), tr("Topology"), tr("Vertices"), tr("Target"), tr("Changed")});
    timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline->setAlternatingRowColors(true);
    timeline->setAccessibleName(tr("Ordered Pica draw-call timeline"));
    timeline->header()->setStretchLastSection(false);
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    event_slider = new QSlider(Qt::Horizontal);
    event_slider->setRange(0, 0);
    event_slider->setEnabled(false);
    event_slider->setAccessibleName(tr("Selected render event"));
    event_slider->setToolTip(tr("Scrub through ordered draw events."));
    event_position = new QLabel(tr("No events"));
    event_position->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("9999 / 9999")));
    event_position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeline_details = new QLabel(tr("Select a draw call to inspect it."));
    timeline_details->setWordWrap(true);
    timeline_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    output_preview = new ScaledPixmapLabel(
        tr("Select a draw call to view its captured output."));
    output_preview->setAlignment(Qt::AlignCenter);
    output_preview->setMinimumSize(0, 0);
    output_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    output_status = new QLabel;
    output_status->setWordWrap(true);
    output_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
        if (render_sessions) {
            UpdateOutputCaptureState();
            const auto active = render_sessions->GetActiveId();
            if (session_selector->count() != static_cast<int>(render_sessions->List().size()) ||
                session_selector->currentData().toULongLong() != active) {
                RefreshSessions(active);
            }
        }
        if (Debugger::IsDockActive(this) && follow_live->isChecked() &&
            !freeze_timeline->isChecked()) {
            RefreshTimeline();
        }
    });
    refresh_timer->start();
    connect(timeline_filter, &QLineEdit::textChanged, this, &GraphicsTracingWidget::FilterTimeline);
    connect(frame_limit, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        Settings::values.render_debugger_frame_limit = static_cast<u32>(value);
        if (render_sessions) {
            render_sessions->GetLive()->SetFrameLimit(static_cast<u32>(value));
        }
        RefreshTimeline();
    });
    connect(timeline, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { SelectTimelineEntry(current); });
    connect(event_slider, &QSlider::valueChanged, this, &GraphicsTracingWidget::SelectEvent);
    connect(timeline, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
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
                    render_sessions->Select(session_selector->itemData(index).toULongLong());
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
    auto* playback_controls = new QHBoxLayout;
    auto* previous_event = new QPushButton(tr("Previous Event"));
    auto* next_event = new QPushButton(tr("Next Event"));
    previous_event->setAccessibleName(tr("Previous render event"));
    next_event->setAccessibleName(tr("Next render event"));
    connect(previous_event, &QPushButton::clicked, event_slider,
            [this] { event_slider->setValue(event_slider->value() - 1); });
    connect(next_event, &QPushButton::clicked, event_slider,
            [this] { event_slider->setValue(event_slider->value() + 1); });
    playback_controls->addWidget(previous_event);
    playback_controls->addWidget(event_slider, 1);
    playback_controls->addWidget(event_position);
    playback_controls->addWidget(next_event);
    main_layout->addLayout(playback_controls);

    auto* inspector = new QWidget;
    auto* inspector_layout = new QVBoxLayout(inspector);
    auto* output_title = new QLabel(tr("Output"));
    QFont output_font = output_title->font();
    output_font.setBold(true);
    output_title->setFont(output_font);
    inspector_layout->addWidget(output_title);
    auto* output_scroll = new QScrollArea;
    output_scroll->setWidgetResizable(true);
    output_scroll->setAlignment(Qt::AlignCenter);
    output_scroll->setWidget(output_preview);
    inspector_layout->addWidget(output_scroll, 1);
    inspector_layout->addWidget(output_status);
    auto* details_title = new QLabel(tr("Details"));
    details_title->setFont(output_font);
    inspector_layout->addWidget(details_title);
    inspector_layout->addWidget(timeline_details);

    auto* content = new QSplitter(Qt::Horizontal);
    content->addWidget(timeline);
    content->addWidget(inspector);
    content->setStretchFactor(0, 2);
    content->setStretchFactor(1, 3);
    content->setChildrenCollapsible(false);
    main_layout->addWidget(content, 1);
    auto* target_controls = new QHBoxLayout;
    target_controls->addWidget(open_color_target);
    target_controls->addWidget(open_depth_target);
    inspector_layout->addLayout(target_controls);
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
    if (!Debugger::IsDockActive(this)) {
        return;
    }
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
        if (auto* current = timeline->currentItem();
            current && current->data(0, Qt::UserRole).isValid()) {
            const int index = current->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(displayed_entries.size())) {
                UpdateOutput(displayed_entries[index].sequence);
            }
        }
        return;
    }

    const auto* current = timeline->currentItem();
    std::optional<u32> selected_sequence;
    if (current && current->data(0, Qt::UserRole).isValid()) {
        const int index = current->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(displayed_entries.size())) {
            selected_sequence = displayed_entries[index].sequence;
        }
    }
    auto* scroll_bar = timeline->verticalScrollBar();
    const int old_scroll = scroll_bar->value();
    const bool was_at_bottom = old_scroll >= scroll_bar->maximum();

    displayed_entries = session->Query({.start = Debugger::RenderSession::Latest, .count = 4096});
    if (displayed_session_id != session_id) {
        displayed_output_sequence.reset();
    }
    displayed_session_id = session_id;
    displayed_status = session->GetStatus();
    have_displayed_status = true;
    timeline->clear();
    displayed_draw_items.clear();
    QTreeWidgetItem* frame_item = nullptr;
    QTreeWidgetItem* target_item = nullptr;
    QTreeWidgetItem* pipeline_item = nullptr;
    u32 displayed_frame = UINT32_MAX;
    QString displayed_target;
    QString displayed_pipeline;
    const QStringList topologies{tr("Triangles"), tr("Triangle strip"), tr("Triangle fan"),
                                 tr("Geometry shader")};
    for (int index = 0; index < static_cast<int>(displayed_entries.size()); ++index) {
        const auto& entry = displayed_entries[index];
        if (!frame_item || displayed_frame != entry.frame) {
            displayed_frame = entry.frame;
            frame_item = new QTreeWidgetItem(timeline, {tr("Frame %1").arg(entry.frame)});
            frame_item->setFirstColumnSpanned(true);
            frame_item->setExpanded(true);
            target_item = nullptr;
            pipeline_item = nullptr;
            displayed_target.clear();
            displayed_pipeline.clear();
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
        const auto draw_details = session->GetDrawDetails(entry.sequence);
        const QString target_key = QStringLiteral("%1:%2:%3:%4")
                                       .arg(entry.target.color_address)
                                       .arg(entry.target.width)
                                       .arg(entry.target.height)
                                       .arg(entry.target.color_format);
        if (!target_item || displayed_target != target_key) {
            displayed_target = target_key;
            displayed_pipeline.clear();
            target_item = new QTreeWidgetItem(
                frame_item, {tr("Render Target 0x%1 · %2x%3 · format %4")
                                 .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                                 .arg(entry.target.width)
                                 .arg(entry.target.height)
                                 .arg(entry.target.color_format)});
            target_item->setData(0, Qt::UserRole + 3, target_item->text(0));
            target_item->setFirstColumnSpanned(true);
            target_item->setExpanded(true);
            pipeline_item = nullptr;
        }
        const QString shader = draw_details && draw_details->shader_id
                                   ? QStringLiteral("0x%1").arg(draw_details->shader_id, 0, 16)
                                   : tr("unavailable");
        const QString pipeline_key = QStringLiteral("%1:%2").arg(shader, topology);
        if (!pipeline_item || displayed_pipeline != pipeline_key) {
            displayed_pipeline = pipeline_key;
            pipeline_item = new QTreeWidgetItem(
                target_item, {tr("Pipeline · VS %1 · %2").arg(shader, topology)});
            pipeline_item->setData(0, Qt::UserRole + 3, pipeline_item->text(0));
            pipeline_item->setFirstColumnSpanned(true);
            pipeline_item->setExpanded(true);
        }
        auto* item = new QTreeWidgetItem(
            pipeline_item, {call, category, topology, QString::number(entry.draw_info.vertex_count),
                            QStringLiteral("0x%1 · %2x%3")
                                .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                                .arg(entry.target.width)
                                .arg(entry.target.height),
                            changed});
        item->setData(0, Qt::UserRole, index);
        item->setData(0, Qt::UserRole + 1, static_cast<int>(displayed_draw_items.size()));
        displayed_draw_items.push_back(item);
        for (auto* group : {target_item, pipeline_item}) {
            const int count = group->data(0, Qt::UserRole + 2).toInt() + 1;
            group->setData(0, Qt::UserRole + 2, count);
            group->setText(0, group->data(0, Qt::UserRole + 3).toString() +
                                  tr(" (%n draw(s))", nullptr, count));
        }
    }
    for (int column = 1; column < timeline->columnCount(); ++column) {
        timeline->resizeColumnToContents(column);
    }
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    {
        const QSignalBlocker blocker{event_slider};
        event_slider->setRange(0, std::max(0, static_cast<int>(displayed_draw_items.size()) - 1));
        event_slider->setEnabled(!displayed_draw_items.empty());
    }
    event_position->setText(displayed_draw_items.empty()
                                ? tr("No events")
                                : tr("%1 events").arg(displayed_draw_items.size()));
    FilterTimeline(timeline_filter->text());
    if (follow_live->isChecked() && was_at_bottom && timeline->topLevelItemCount()) {
        if (!displayed_draw_items.empty()) {
            timeline->setCurrentItem(displayed_draw_items.back());
            timeline->scrollToItem(timeline->currentItem());
        }
    } else {
        if (selected_sequence) {
            for (auto* item : displayed_draw_items) {
                const int index = item->data(0, Qt::UserRole).toInt();
                if (index >= 0 && index < static_cast<int>(displayed_entries.size()) &&
                    displayed_entries[index].sequence == *selected_sequence) {
                    timeline->setCurrentItem(item);
                    break;
                }
            }
        }
        scroll_bar->setValue(old_scroll);
    }
}

void GraphicsTracingWidget::FilterTimeline(const QString& text) {
    const QString needle = text.trimmed();
    const auto filter = [&](auto&& self, QTreeWidgetItem* item, bool parent_matches) -> bool {
        bool matches = parent_matches || needle.isEmpty();
        for (int column = 0; !matches && column < timeline->columnCount(); ++column) {
            matches = item->text(column).contains(needle, Qt::CaseInsensitive);
        }
        bool child_visible = false;
        for (int child = 0; child < item->childCount(); ++child) {
            child_visible |= self(self, item->child(child), matches);
        }
        const bool visible = matches || child_visible;
        item->setHidden(!visible);
        return visible;
    };
    for (int frame = 0; frame < timeline->topLevelItemCount(); ++frame) {
        filter(filter, timeline->topLevelItem(frame), false);
    }
}

void GraphicsTracingWidget::SelectTimelineEntry(QTreeWidgetItem* current) {
    selected_target.reset();
    if (!current || !current->data(0, Qt::UserRole).isValid()) {
        displayed_output_sequence.reset();
        timeline_details->setText(tr("Select a draw call to inspect it."));
        output_preview->SetPixmap({});
        output_preview->setText(tr("Select a draw call to view its captured output."));
        output_status->clear();
        open_color_target->setEnabled(false);
        open_depth_target->setEnabled(false);
        return;
    }
    const int index = current->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(displayed_entries.size())) {
        return;
    }
    const auto& entry = displayed_entries[index];
    {
        const QSignalBlocker blocker{event_slider};
        event_slider->setValue(current->data(0, Qt::UserRole + 1).toInt());
    }
    event_position->setText(tr("%1 / %2")
                                .arg(current->data(0, Qt::UserRole + 1).toInt() + 1)
                                .arg(displayed_draw_items.size()));
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
    const auto draw_details = render_sessions->GetActive()->GetDrawDetails(entry.sequence);
    if (draw_details) {
        timeline_details->setText(
            timeline_details->text() +
            tr("\nRegister writes %1 · shader %2 · resources %3")
                .arg(draw_details->write_count)
                .arg(draw_details->shader_id
                         ? QStringLiteral("0x%1").arg(draw_details->shader_id, 0, 16)
                         : tr("unavailable"))
                .arg(draw_details->resource_count));
    }
    const bool live = render_sessions && render_sessions->GetActiveId() ==
                                             Debugger::RenderSessionManager::LiveSessionId;
    open_color_target->setEnabled(live && entry.target.color_address != 0);
    open_depth_target->setEnabled(live && entry.target.depth_address != 0);
    if (!live) {
        timeline_details->setText(timeline_details->text() +
                                  tr("\nImported resource bytes are retained; the surface viewer "
                                     "currently reads live memory only."));
    }
    UpdateOutput(entry.sequence);
}

void GraphicsTracingWidget::SelectEvent(int event) {
    if (event < 0 || event >= static_cast<int>(displayed_draw_items.size())) {
        return;
    }
    auto* item = displayed_draw_items[event];
    if (item->isHidden()) {
        return;
    }
    timeline->setCurrentItem(item);
    timeline->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void GraphicsTracingWidget::UpdateOutputCaptureState() {
    render_sessions->GetLive()->SetOutputCaptureEnabled(
        isVisible() && Debugger::IsDockActive(this) &&
        render_sessions->GetActiveId() == Debugger::RenderSessionManager::LiveSessionId);
}

void GraphicsTracingWidget::UpdateOutput(Debugger::u32 sequence) {
    if (!render_sessions || displayed_output_sequence == sequence) {
        return;
    }
    const auto resource = render_sessions->GetActive()->GetDrawOutput(sequence);
    if (!resource) {
        output_preview->SetPixmap({});
        output_preview->setText(tr("Output was not captured for this draw."));
        output_status->setText(
            render_sessions->GetActiveId() == Debugger::RenderSessionManager::LiveSessionId
                ? tr("Output capture is available while the live debugger is enabled and visible.")
                : tr("This imported capture does not contain output pixels for this draw."));
        return;
    }
    if (resource->bytes.empty()) {
        output_preview->SetPixmap({});
        output_preview->setText(tr("Output data is not available for this draw."));
        output_status->setText(tr("The render target metadata was captured without pixel data."));
        return;
    }
    displayed_output_sequence = sequence;
    const QImage image = DecodeColorTarget(*resource);
    if (image.isNull()) {
        output_preview->SetPixmap({});
        output_preview->setText(tr("Captured output cannot be decoded."));
        output_status->setText(tr("%1x%2 · format %3 · %4 bytes")
                                   .arg(resource->width)
                                   .arg(resource->height)
                                   .arg(resource->format)
                                   .arg(resource->bytes.size()));
        return;
    }
    output_preview->setText({});
    output_preview->SetPixmap(QPixmap::fromImage(image));
    output_status->setText(tr("%1x%2 · format %3 · address 0x%4 · %5 bytes")
                               .arg(resource->width)
                               .arg(resource->height)
                               .arg(resource->format)
                               .arg(resource->address, 0, 16)
                               .arg(resource->bytes.size()));
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
    auto* viewer = new GraphicsSurfaceWidget(system, context_weak.lock(), dockManager());
    Debugger::ConfigureDockWorkspace(viewer);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    dockManager()->addDockWidgetFloating(viewer);
    viewer->ViewRenderTarget(*selected_target, depth);
    viewer->resize(640, 520);
    viewer->toggleView(true);
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
    limits.owned_bytes = limits.total_bytes;
    std::string error;
    if (!Debugger::LoadCapture(filename.toStdString(), capture, error, limits)) {
        QMessageBox::critical(this, tr("Capture import failed"), QString::fromStdString(error));
        return;
    }
    const auto id = render_sessions->AddImported(std::move(capture));
    if (!id) {
        QMessageBox::critical(
            this, tr("Capture import failed"),
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
        const QString name = descriptor.live ? tr("Live — %1/%2")
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
