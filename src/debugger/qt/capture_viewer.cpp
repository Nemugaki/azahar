// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#include "capture_viewer.h"

#include <algorithm>
#include <array>

#include <QBoxLayout>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>

#include "render_debugger/capture_file.h"

namespace Debugger {
namespace {

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
            const u64 offset = (y / 8) * stride + (x / 8) * tile_size +
                               Morton(x, y) * pixel_size;
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
    auto* refresh = new QPushButton{tr("Refresh")};
    filter_row->addWidget(new QLabel{tr("Filter:")});
    filter_row->addWidget(filter, 1);
    filter_row->addWidget(refresh);
    layout->addLayout(filter_row);
    load_older = new QPushButton{tr("Load 128 older events")};
    layout->addWidget(load_older);

    timeline = new QTreeWidget;
    timeline->setColumnCount(5);
    timeline->setHeaderLabels(
        {tr("Call"), tr("Topology"), tr("Vertices"), tr("Target"), tr("Changed")});
    timeline->setAlternatingRowColors(true);
    timeline->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* inspector = new QWidget;
    auto* inspector_layout = new QVBoxLayout{inspector};
    preview = new QLabel{tr("Select a draw call to view its captured output.")};
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(240, 160);
    preview->setScaledContents(false);
    output_status = new QLabel;
    output_status->setWordWrap(true);
    details = new QLabel{tr("Select a draw call to inspect it.")};
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspector_layout->addWidget(preview, 1);
    inspector_layout->addWidget(output_status);
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
    connect(open_color, &QPushButton::clicked, this,
            [this] { emit OpenTargetRequested(false); });
    connect(open_depth, &QPushButton::clicked, this,
            [this] { emit OpenTargetRequested(true); });
    connect(timeline, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) { SelectEntry(item); });
    connect(filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int index = 0; index < timeline->topLevelItemCount(); ++index) {
            auto* item = timeline->topLevelItem(index);
            bool visible = text.isEmpty();
            for (int column = 0; !visible && column < timeline->columnCount(); ++column) {
                visible = item->text(column).contains(text, Qt::CaseInsensitive);
            }
            item->setHidden(!visible);
        }
    });
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
        if (active) {
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
        session_selector->addItem(
            descriptor.live ? tr("Live — %1/%2").arg(QString::fromStdString(descriptor.producer),
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
        return;
    }
    displayed_session = id;
    displayed_status = status;
    entries = session->Query({.start = RenderSession::Latest, .count = displayed_limit});
    load_older->setVisible(entries.size() < status.count);
    timeline->clear();
    const QStringList topologies{tr("Triangles"), tr("Triangle strip"), tr("Triangle fan"),
                                 tr("Geometry shader")};
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const auto& entry = entries[index];
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
        auto* item = new QTreeWidgetItem{
            timeline,
            {tr("Frame %1 · draw %2").arg(entry.frame).arg(entry.draw),
             entry.draw_info.topology < topologies.size() ? topologies[entry.draw_info.topology]
                                                          : tr("Unknown"),
             QString::number(entry.draw_info.vertex_count),
             QStringLiteral("0x%1 · %2x%3")
                 .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                 .arg(entry.target.width)
                 .arg(entry.target.height),
             changed.join(QStringLiteral(", "))}};
        item->setData(0, Qt::UserRole, index);
    }
    filter->textChanged(filter->text());
}

void CaptureViewerWidget::SelectEntry(QTreeWidgetItem* item) {
    selected_target.reset();
    open_color->setEnabled(false);
    open_depth->setEnabled(false);
    if (!item) {
        return;
    }
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(entries.size())) {
        return;
    }
    const auto& entry = entries[index];
    selected_target = entry.target;
    const auto draw = sessions->GetActive()->GetDrawDetails(entry.sequence);
    details->setText(tr("Sequence %1 · frame %2 · draw %3 · VS entry 0x%4\n"
                        "Color 0x%5 · depth 0x%6 · %7x%8\n"
                        "Register writes %9 · shader %10 · resources %11")
                         .arg(entry.sequence)
                         .arg(entry.frame)
                         .arg(entry.draw)
                         .arg(entry.draw_info.vertex_shader_entry, 0, 16)
                         .arg(entry.target.color_address, 8, 16, QLatin1Char('0'))
                         .arg(entry.target.depth_address, 8, 16, QLatin1Char('0'))
                         .arg(entry.target.width)
                         .arg(entry.target.height)
                         .arg(draw ? draw->write_count : 0)
                         .arg(draw && draw->shader_id
                                  ? QStringLiteral("0x%1").arg(draw->shader_id, 0, 16)
                                  : tr("unavailable"))
                         .arg(draw ? draw->resource_count : 0));
    const bool live = sessions->GetActiveId() == RenderSessionManager::LiveSessionId;
    open_color->setEnabled(live && entry.target.color_address);
    open_depth->setEnabled(live && entry.target.depth_address);
    UpdateOutput(entry.sequence);
}

void CaptureViewerWidget::UpdateOutput(u32 sequence) {
    const auto resource = sessions->GetActive()->GetDrawOutput(sequence);
    if (!resource) {
        preview->setPixmap({});
        preview->setText(tr("Output was not captured for this draw."));
        output_status->clear();
        return;
    }
    const QImage image = Decode(*resource);
    if (image.isNull()) {
        preview->setPixmap({});
        preview->setText(tr("Captured output cannot be decoded."));
    } else {
        preview->setText({});
        preview->setPixmap(QPixmap::fromImage(image).scaled(
            preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    output_status->setText(tr("%1x%2 · format %3 · address 0x%4 · %5 bytes")
                               .arg(resource->width)
                               .arg(resource->height)
                               .arg(resource->format)
                               .arg(resource->address, 0, 16)
                               .arg(resource->bytes.size()));
}

void CaptureViewerWidget::ImportCapture() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Render Debugger Capture"), {}, tr("Render Debugger Capture (*.rdbg)"));
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
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Render Debugger Capture"), QStringLiteral("capture.rdbg"),
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
