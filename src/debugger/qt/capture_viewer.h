// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QWidget>

#include "render_debugger/render_session.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;

namespace Debugger {

class CaptureViewerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CaptureViewerWidget(std::shared_ptr<RenderSessionManager> sessions,
                                 QWidget* parent = nullptr);

    void Refresh();
    void SetActive(bool active);
    std::optional<RenderTarget> SelectedTarget() const;

signals:
    void OpenTargetRequested(bool depth);

private:
    void RefreshSessions(u64 selected = 0);
    void FilterTimeline(const QString& text);
    void SelectEntry(QTreeWidgetItem* item);
    void SelectEvent(int event);
    void UpdateOutput(u32 sequence);
    void ImportCapture();
    void ExportCapture();
    void RemoveCapture();

    std::shared_ptr<RenderSessionManager> sessions;
    QComboBox* session_selector{};
    QCheckBox* follow_live{};
    QCheckBox* freeze_timeline{};
    QLineEdit* filter{};
    QTreeWidget* timeline{};
    QSlider* event_slider{};
    QLabel* event_position{};
    QLabel* details{};
    QLabel* preview{};
    QLabel* output_status{};
    QPushButton* remove_capture{};
    QPushButton* load_older{};
    QPushButton* open_color{};
    QPushButton* open_depth{};
    std::vector<TimelineEntry> entries;
    std::vector<QTreeWidgetItem*> draw_items;
    std::optional<RenderTarget> selected_target;
    std::optional<u32> displayed_output_sequence;
    TimelineStatus displayed_status{};
    u64 displayed_session{};
    u32 displayed_limit{128};
    bool active{};
};

} // namespace Debugger
