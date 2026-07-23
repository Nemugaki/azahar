// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include "capture_viewer.h"

#include <algorithm>
#include <array>
#include <limits>

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
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>

#include "render_debugger/capture_file.h"

namespace Debugger {
namespace {

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

constexpr u32 Morton(u32 x, u32 y) {
    constexpr std::array<u32, 8> xs{0, 1, 4, 5, 16, 17, 20, 21};
    constexpr std::array<u32, 8> ys{0, 2, 8, 10, 32, 34, 40, 42};
    return xs[x & 7] + ys[y & 7];
}

u8 Expand(u32 value, u32 bits) {
    return static_cast<u8>(value * 255 / ((1U << bits) - 1));
}

QImage Decode(const Resource& resource) {
    static constexpr std::array<u32, 5> bytes_per_pixel{4, 3, 2, 2, 2};
    if (!resource.width || !resource.height || resource.format >= bytes_per_pixel.size()) {
        return {};
    }
    const u32 pixel_size = bytes_per_pixel[resource.format];
    const u64 tile_size = 64 * pixel_size;
    const u64 stride = ((resource.width + 7) / 8) * tile_size;
    if (stride * ((resource.height + 7) / 8) > resource.bytes.size()) {
        return {};
    }
    QImage image(resource.width, resource.height, QImage::Format_ARGB32);
    for (u32 y = 0; y < resource.height; ++y) {
        for (u32 x = 0; x < resource.width; ++x) {
            const u64 offset = (y / 8) * stride + (x / 8) * tile_size + Morton(x, y) * pixel_size;
            const auto* source = resource.bytes.data() + offset;
            u32 red{}, green{}, blue{}, alpha{255};
            if (resource.format == 0) {
                red = source[3];
                green = source[2];
                blue = source[1];
                alpha = source[0];
            } else if (resource.format == 1) {
                red = source[2];
                green = source[1];
                blue = source[0];
            } else {
                const u32 pixel = source[0] | static_cast<u32>(source[1]) << 8;
                if (resource.format == 2) {
                    red = Expand(pixel >> 11, 5);
                    green = Expand((pixel >> 6) & 31, 5);
                    blue = Expand((pixel >> 1) & 31, 5);
                    alpha = pixel & 1 ? 255 : 0;
                } else if (resource.format == 3) {
                    red = Expand(pixel >> 11, 5);
                    green = Expand((pixel >> 5) & 63, 6);
                    blue = Expand(pixel & 31, 5);
                } else {
                    red = Expand(pixel >> 12, 4);
                    green = Expand((pixel >> 8) & 15, 4);
                    blue = Expand((pixel >> 4) & 15, 4);
                    alpha = Expand(pixel & 15, 4);
                }
            }
            image.setPixel(x, y, qRgba(red, green, blue, alpha));
        }
    }
    return image;
}

} // namespace

CaptureViewerWidget::CaptureViewerWidget(std::shared_ptr<RenderSessionManager> sessions_,
                                         QWidget* parent)
    : QWidget{parent}, sessions{std::move(sessions_)} {
    auto* layout = new QVBoxLayout{this};
    auto* session_row = new QHBoxLayout;
    session_selector = new QComboBox;
    auto* import_capture = new QPushButton{tr("Import Capture")};
    auto* export_capture = new QPushButton{tr("Export Capture")};
    remove_capture = new QPushButton{tr("Remove Capture")};
    session_row->addWidget(new QLabel{tr("Session:")});
    session_row->addWidget(session_selector, 1);
    session_row->addWidget(import_capture);
    session_row->addWidget(export_capture);
    session_row->addWidget(remove_capture);
    layout->addLayout(session_row);

    auto* filter_row = new QHBoxLayout;
    filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter draws, frames, targets, or topology"));
    filter->setClearButtonEnabled(true);
    follow_live = new QCheckBox{tr("Follow live")};
    follow_live->setChecked(true);
    freeze_timeline = new QCheckBox{tr("Freeze selection")};
    freeze_timeline->setObjectName(QStringLiteral("freezeRenderSelection"));
    freeze_timeline->setToolTip(
        tr("Keeps the selected event while the bounded live timeline continues to refresh."));
    auto* refresh = new QPushButton{tr("Refresh")};
    filter_row->addWidget(new QLabel{tr("Filter:")});
    filter_row->addWidget(filter, 1);
    filter_row->addWidget(follow_live);
    filter_row->addWidget(freeze_timeline);
    filter_row->addWidget(refresh);
    layout->addLayout(filter_row);
    load_older = new QPushButton{tr("Load 128 older events")};
    layout->addWidget(load_older);

    timeline = new QTreeWidget;
    timeline->setObjectName(QStringLiteral("renderTimeline"));
    timeline->setColumnCount(6);
    timeline->setHeaderLabels(
        {tr("Call"), tr("Category"), tr("Topology"), tr("Vertices"), tr("Target"), tr("Changed")});
    timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline->setAlternatingRowColors(true);
    timeline->setAccessibleName(tr("Ordered render draw-call timeline"));
    timeline->header()->setStretchLastSection(false);
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    auto* playback = new QHBoxLayout;
    auto* previous_event = new QPushButton{tr("Previous Event")};
    auto* next_event = new QPushButton{tr("Next Event")};
    event_slider = new QSlider{Qt::Horizontal};
    event_slider->setObjectName(QStringLiteral("renderEventSlider"));
    event_slider->setRange(0, 0);
    event_slider->setEnabled(false);
    event_slider->setAccessibleName(tr("Selected render event"));
    event_slider->setToolTip(tr("Scrub through ordered draw events."));
    event_position = new QLabel{tr("No events")};
    event_position->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("9999 / 9999")));
    event_position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    playback->addWidget(previous_event);
    playback->addWidget(event_slider, 1);
    playback->addWidget(event_position);
    playback->addWidget(next_event);
    layout->addLayout(playback);

    auto* inspector = new QWidget;
    auto* inspector_layout = new QVBoxLayout{inspector};
    auto* output_title = new QLabel{tr("Output")};
    QFont heading = output_title->font();
    heading.setBold(true);
    output_title->setFont(heading);
    inspector_layout->addWidget(output_title);
    preview = new ScaledPixmapLabel{tr("Select a draw call to view its captured output.")};
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(0, 0);
    preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* output_scroll = new QScrollArea;
    output_scroll->setWidgetResizable(true);
    output_scroll->setAlignment(Qt::AlignCenter);
    output_scroll->setWidget(preview);
    inspector_layout->addWidget(output_scroll, 1);
    output_status = new QLabel;
    output_status->setWordWrap(true);
    output_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspector_layout->addWidget(output_status);
    auto* details_title = new QLabel{tr("Details")};
    details_title->setFont(heading);
    inspector_layout->addWidget(details_title);
    details = new QLabel{tr("Select a draw call to inspect it.")};
    details->setObjectName(QStringLiteral("renderEventDetails"));
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspector_layout->addWidget(details);
    auto* targets = new QHBoxLayout;
    open_color = new QPushButton{tr("Open Color Target")};
    open_depth = new QPushButton{tr("Open Depth Target")};
    targets->addWidget(open_color);
    targets->addWidget(open_depth);
    inspector_layout->addLayout(targets);
    auto* split = new QSplitter;
    split->addWidget(timeline);
    split->addWidget(inspector);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);

    connect(refresh, &QPushButton::clicked, this, &CaptureViewerWidget::Refresh);
    connect(load_older, &QPushButton::clicked, this, [this] {
        displayed_limit += 128;
        displayed_session = 0;
        Refresh();
    });
    connect(import_capture, &QPushButton::clicked, this, &CaptureViewerWidget::ImportCapture);
    connect(export_capture, &QPushButton::clicked, this, &CaptureViewerWidget::ExportCapture);
    connect(remove_capture, &QPushButton::clicked, this, &CaptureViewerWidget::RemoveCapture);
    connect(open_color, &QPushButton::clicked, this, [this] { emit OpenTargetRequested(false); });
    connect(open_depth, &QPushButton::clicked, this, [this] { emit OpenTargetRequested(true); });
    connect(previous_event, &QPushButton::clicked, event_slider,
            [this] { event_slider->setValue(event_slider->value() - 1); });
    connect(next_event, &QPushButton::clicked, event_slider,
            [this] { event_slider->setValue(event_slider->value() + 1); });
    connect(event_slider, &QSlider::valueChanged, this, &CaptureViewerWidget::SelectEvent);
    connect(timeline, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) { SelectEntry(item); });
    connect(filter, &QLineEdit::textChanged, this, &CaptureViewerWidget::FilterTimeline);
    connect(session_selector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 0 && sessions) {
                    sessions->Select(session_selector->itemData(index).toULongLong());
                    displayed_limit = 128;
                    displayed_session = 0;
                    SetActive(active);
                    Refresh();
                }
            });
    auto* timer = new QTimer{this};
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!active || !follow_live->isChecked()) {
            return;
        }
        const auto status = sessions->GetActive()->GetStatus();
        if (!freeze_timeline->isChecked() ||
            status.oldest_sequence != displayed_status.oldest_sequence) {
            Refresh();
        }
    });
    timer->start();
    RefreshSessions();
    Refresh();
}

void CaptureViewerWidget::SetActive(bool value) {
    active = value;
    if (sessions) {
        sessions->GetLive()->SetOutputCaptureEnabled(
            RenderSession::CaptureOwner::UserInterface,
            active && sessions->GetActiveId() == RenderSessionManager::LiveSessionId);
    }
}

std::optional<RenderTarget> CaptureViewerWidget::SelectedTarget() const {
    return selected_target;
}

void CaptureViewerWidget::RefreshSessions(u64 selected) {
    if (!sessions) {
        return;
    }
    selected = selected ? selected : sessions->GetActiveId();
    const QSignalBlocker blocker{session_selector};
    session_selector->clear();
    int current{};
    for (const auto& descriptor : sessions->List()) {
        session_selector->addItem(descriptor.live
                                      ? tr("Live — %1/%2")
                                            .arg(QString::fromStdString(descriptor.producer),
                                                 QString::fromStdString(descriptor.backend))
                                      : tr("Imported — %1/%2")
                                            .arg(QString::fromStdString(descriptor.producer),
                                                 QString::fromStdString(descriptor.backend)),
                                  QVariant::fromValue<qulonglong>(descriptor.id));
        if (descriptor.id == selected) {
            current = session_selector->count() - 1;
        }
    }
    session_selector->setCurrentIndex(current);
    sessions->Select(selected);
    remove_capture->setEnabled(selected != RenderSessionManager::LiveSessionId);
}

void CaptureViewerWidget::Refresh() {
    if (!sessions) {
        return;
    }
    if (session_selector->count() != static_cast<int>(sessions->List().size())) {
        RefreshSessions();
    }
    const auto session = sessions->GetActive();
    const auto status = session->GetStatus();
    const u64 id = sessions->GetActiveId();
    if (id == displayed_session && status.count == displayed_status.count &&
        status.oldest_sequence == displayed_status.oldest_sequence &&
        status.newest_sequence == displayed_status.newest_sequence &&
        status.truncated == displayed_status.truncated) {
        if (auto* current = timeline->currentItem();
            current && current->data(0, Qt::UserRole).isValid()) {
            const int index = current->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(entries.size())) {
                UpdateOutput(entries[index].sequence);
            }
        }
        return;
    }

    std::optional<u32> selected_sequence;
    if (const auto* current = timeline->currentItem();
        current && current->data(0, Qt::UserRole).isValid()) {
        const int index = current->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(entries.size())) {
            selected_sequence = entries[index].sequence;
        }
    }
    auto* scroll_bar = timeline->verticalScrollBar();
    const int old_scroll = scroll_bar->value();
    const bool was_at_bottom = old_scroll >= scroll_bar->maximum();

    if (displayed_session != id) {
        displayed_output_sequence.reset();
    }
    displayed_session = id;
    displayed_status = status;
    entries = session->Query({.start = RenderSession::Latest, .count = displayed_limit});
    load_older->setVisible(entries.size() < status.count);
    timeline->clear();
    draw_items.clear();
    QTreeWidgetItem* frame_item{};
    QTreeWidgetItem* target_item{};
    QTreeWidgetItem* pipeline_item{};
    u32 displayed_frame = std::numeric_limits<u32>::max();
    QString displayed_target;
    QString displayed_pipeline;
    const QStringList topologies{tr("Triangles"), tr("Triangle strip"), tr("Triangle fan"),
                                 tr("Geometry shader")};
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const auto& entry = entries[index];
        if (!frame_item || displayed_frame != entry.frame) {
            displayed_frame = entry.frame;
            frame_item = new QTreeWidgetItem{timeline, {tr("Frame %1").arg(entry.frame)}};
            frame_item->setFirstColumnSpanned(true);
            frame_item->setExpanded(true);
            target_item = nullptr;
            pipeline_item = nullptr;
            displayed_target.clear();
            displayed_pipeline.clear();
        }
        if (entry.kind != TimelineKind::Draw) {
            continue;
        }
        QStringList changed;
        if (entry.changed_mask & 1)
            changed << tr("Color");
        if (entry.changed_mask & 2)
            changed << tr("Depth");
        if (entry.changed_mask & 4)
            changed << tr("Size");
        if (entry.changed_mask & 8)
            changed << tr("Format");
        const QString call = [&] {
            switch (entry.draw_info.mode) {
            case DrawMode::Indexed:
                return tr("Draw %1 — indexed").arg(entry.draw);
            case DrawMode::Immediate:
                return tr("Draw %1 — immediate").arg(entry.draw);
            default:
                return tr("Draw %1 — arrays").arg(entry.draw);
            }
        }();
        const QString category = entry.changed_mask & 3 ? tr("Render target") : tr("Geometry");
        const QString topology = entry.draw_info.topology < topologies.size()
                                     ? topologies[entry.draw_info.topology]
                                     : tr("Unknown");
        const auto draw = session->GetDrawDetails(entry.sequence);
        const QString target_key = QStringLiteral("%1:%2:%3:%4:%5:%6")
                                       .arg(entry.target.color_address)
                                       .arg(entry.target.depth_address)
                                       .arg(entry.target.width)
                                       .arg(entry.target.height)
                                       .arg(entry.target.color_format)
                                       .arg(entry.target.depth_format);
        if (!target_item || displayed_target != target_key) {
            displayed_target = target_key;
            displayed_pipeline.clear();
            target_item =
                new QTreeWidgetItem{frame_item,
                                    {tr("Render Target 0x%1 · depth 0x%2 · %3x%4 · formats %5/%6")
                                         .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                                         .arg(entry.target.depth_address, 8, 16, QLatin1Char('0'))
                                         .arg(entry.target.width)
                                         .arg(entry.target.height)
                                         .arg(entry.target.color_format)
                                         .arg(entry.target.depth_format)}};
            target_item->setData(0, Qt::UserRole + 3, target_item->text(0));
            target_item->setFirstColumnSpanned(true);
            target_item->setExpanded(true);
            pipeline_item = nullptr;
        }
        const QString shader = draw && draw->shader_id
                                   ? QStringLiteral("0x%1").arg(draw->shader_id, 0, 16)
                                   : tr("unavailable");
        const QString pipeline_key = QStringLiteral("%1:%2").arg(shader, topology);
        if (!pipeline_item || displayed_pipeline != pipeline_key) {
            displayed_pipeline = pipeline_key;
            pipeline_item = new QTreeWidgetItem{
                target_item, {tr("Pipeline · VS %1 · %2").arg(shader, topology)}};
            pipeline_item->setData(0, Qt::UserRole + 3, pipeline_item->text(0));
            pipeline_item->setFirstColumnSpanned(true);
            pipeline_item->setExpanded(true);
        }
        auto* item = new QTreeWidgetItem{
            pipeline_item,
            {call, category, topology, QString::number(entry.draw_info.vertex_count),
             QStringLiteral("0x%1 · %2x%3")
                 .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                 .arg(entry.target.width)
                 .arg(entry.target.height),
             changed.join(QStringLiteral(", "))}};
        item->setData(0, Qt::UserRole, index);
        item->setData(0, Qt::UserRole + 1, static_cast<int>(draw_items.size()));
        draw_items.push_back(item);
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
        event_slider->setRange(0, std::max(0, static_cast<int>(draw_items.size()) - 1));
        event_slider->setEnabled(!draw_items.empty());
    }
    event_position->setText(draw_items.empty() ? tr("No events")
                                               : tr("%1 events").arg(draw_items.size()));
    FilterTimeline(filter->text());
    if (follow_live->isChecked() && !freeze_timeline->isChecked() && was_at_bottom &&
        !draw_items.empty()) {
        timeline->setCurrentItem(draw_items.back());
        timeline->scrollToItem(draw_items.back());
    } else {
        bool restored = false;
        if (selected_sequence) {
            for (auto* item : draw_items) {
                const int index = item->data(0, Qt::UserRole).toInt();
                if (index >= 0 && index < static_cast<int>(entries.size()) &&
                    entries[index].sequence == *selected_sequence) {
                    timeline->setCurrentItem(item);
                    restored = true;
                    break;
                }
            }
        }
        if (!restored && !draw_items.empty()) {
            timeline->setCurrentItem(freeze_timeline->isChecked() ? draw_items.front()
                                                                  : draw_items.back());
        }
        scroll_bar->setValue(old_scroll);
    }
}

void CaptureViewerWidget::FilterTimeline(const QString& text) {
    const QString needle = text.trimmed();
    const auto filter_item = [&](auto&& self, QTreeWidgetItem* item, bool parent_matches) -> bool {
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
        filter_item(filter_item, timeline->topLevelItem(frame), false);
    }
}

void CaptureViewerWidget::SelectEntry(QTreeWidgetItem* item) {
    selected_target.reset();
    open_color->setEnabled(false);
    open_depth->setEnabled(false);
    if (!item || !item->data(0, Qt::UserRole).isValid()) {
        displayed_output_sequence.reset();
        details->setText(tr("Select a draw call to inspect it."));
        static_cast<ScaledPixmapLabel*>(preview)->SetPixmap({});
        preview->setText(tr("Select a draw call to view its captured output."));
        output_status->clear();
        return;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return;
    }
    const auto& entry = entries[index];
    {
        const QSignalBlocker blocker{event_slider};
        event_slider->setValue(item->data(0, Qt::UserRole + 1).toInt());
    }
    event_position->setText(
        tr("%1 / %2").arg(item->data(0, Qt::UserRole + 1).toInt() + 1).arg(draw_items.size()));
    selected_target = entry.target;
    const auto draw = sessions->GetActive()->GetDrawDetails(entry.sequence);
    details->setText(tr("Sequence %1 · frame %2 · draw %3 · frame draw %4\n"
                        "Vertices %5 at offset %6 · topology %7 · VS entry 0x%8\n"
                        "Color 0x%9 (format %10) · depth 0x%11 (format %12) · %13x%14")
                         .arg(entry.sequence)
                         .arg(entry.frame)
                         .arg(entry.draw)
                         .arg(entry.frame_draw)
                         .arg(entry.draw_info.vertex_count)
                         .arg(entry.draw_info.vertex_offset)
                         .arg(entry.draw_info.topology)
                         .arg(entry.draw_info.vertex_shader_entry, 0, 16)
                         .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                         .arg(entry.target.color_format)
                         .arg(entry.target.depth_address, 8, 16, QLatin1Char('0'))
                         .arg(entry.target.depth_format)
                         .arg(entry.target.width)
                         .arg(entry.target.height));
    if (draw) {
        details->setText(details->text() + tr("\nRegister writes %1 · shader %2 · resources %3")
                                               .arg(draw->write_count)
                                               .arg(draw->shader_id ? QStringLiteral("0x%1").arg(
                                                                          draw->shader_id, 0, 16)
                                                                    : tr("unavailable"))
                                               .arg(draw->resource_count));
    }
    if (const auto state = sessions->GetActive()->GetDrawState(entry.sequence)) {
        const auto nonzero = std::ranges::count_if(*state, [](u32 value) { return value != 0; });
        details->setText(details->text() + tr("\nImmutable render state: %1 registers (%2 nonzero)")
                                               .arg(state->size())
                                               .arg(nonzero));
    }
    const bool live = sessions->GetActiveId() == RenderSessionManager::LiveSessionId;
    open_color->setEnabled(live && entry.target.color_address);
    open_depth->setEnabled(live && entry.target.depth_address);
    if (!live) {
        details->setText(details->text() +
                         tr("\nImported resource bytes are retained; target buttons require a "
                            "live-memory adapter."));
    }
    UpdateOutput(entry.sequence);
}

void CaptureViewerWidget::SelectEvent(int event) {
    if (event < 0 || event >= static_cast<int>(draw_items.size())) {
        return;
    }
    auto* item = draw_items[event];
    if (item->isHidden()) {
        return;
    }
    timeline->setCurrentItem(item);
    timeline->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void CaptureViewerWidget::UpdateOutput(u32 sequence) {
    if (displayed_output_sequence == sequence) {
        return;
    }
    const auto resource = sessions->GetActive()->GetDrawOutput(sequence);
    if (!resource) {
        static_cast<ScaledPixmapLabel*>(preview)->SetPixmap({});
        preview->setText(tr("Output was not captured for this draw."));
        const bool live = sessions->GetActiveId() == RenderSessionManager::LiveSessionId;
        const auto status = sessions->GetActive()->GetStatus();
        output_status->setText(
            live && sequence < status.oldest_sequence
                ? tr("This event has expired from the bounded live capture.")
            : live ? tr("Output capture is available while the live viewer is active.")
                   : tr("This imported capture does not contain output pixels for this draw."));
        return;
    }
    displayed_output_sequence = sequence;
    const QImage image = Decode(*resource);
    if (image.isNull()) {
        static_cast<ScaledPixmapLabel*>(preview)->SetPixmap({});
        preview->setText(tr("Captured output cannot be decoded."));
    } else {
        preview->setText({});
        static_cast<ScaledPixmapLabel*>(preview)->SetPixmap(QPixmap::fromImage(image));
    }
    output_status->setText(tr("%1x%2 · format %3 · address 0x%4 · %5 bytes")
                               .arg(resource->width)
                               .arg(resource->height)
                               .arg(resource->format)
                               .arg(resource->address, 0, 16)
                               .arg(resource->bytes.size()));
}

void CaptureViewerWidget::ImportCapture() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Render Debugger Capture"),
                                                      {}, tr("Render Debugger Capture (*.rdbg)"));
    if (path.isEmpty()) {
        return;
    }
    Capture capture;
    std::string error;
    if (!LoadCapture(path.toStdString(), capture, error)) {
        QMessageBox::critical(this, tr("Capture import failed"), QString::fromStdString(error));
        return;
    }
    const u64 id = sessions->AddImported(std::move(capture));
    if (!id) {
        QMessageBox::critical(this, tr("Capture import failed"), tr("The capture is invalid."));
        return;
    }
    RefreshSessions(id);
    SetActive(active);
    displayed_session = 0;
    Refresh();
}

void CaptureViewerWidget::ExportCapture() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Render Debugger Capture"),
                                                      QStringLiteral("capture.rdbg"),
                                                      tr("Render Debugger Capture (*.rdbg)"));
    if (path.isEmpty()) {
        return;
    }
    Capture capture;
    std::string error;
    if (!sessions->Snapshot(sessions->GetActiveId(), capture) ||
        !SaveCapture(path.toStdString(), capture, error)) {
        QMessageBox::critical(this, tr("Capture export failed"), QString::fromStdString(error));
    }
}

void CaptureViewerWidget::RemoveCapture() {
    if (sessions->Remove(sessions->GetActiveId())) {
        displayed_session = 0;
        RefreshSessions(RenderSessionManager::LiveSessionId);
        SetActive(active);
        Refresh();
    }
}

} // namespace Debugger
