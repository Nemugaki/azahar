// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <vector>
#include "citra_qt/debugger/graphics/graphics_breakpoint_observer.h"
#include "debugger/render_session.h"

namespace Core {
class System;
}

class EmuThread;
class QLabel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;
class ScaledPixmapLabel;

class GraphicsTracingWidget : public BreakPointObserverDock {
    Q_OBJECT

public:
    explicit GraphicsTracingWidget(Core::System& system,
                                   std::shared_ptr<Pica::DebugContext> debug_context,
                                   QWidget* parent = nullptr);

    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

private slots:
    void StartRecording();
    void StopRecording();
    void AbortRecording();
    void RefreshTimeline();
    void FilterTimeline(const QString& text);
    void SelectTimelineEntry(QTreeWidgetItem* current);
    void OpenColorTarget();
    void OpenDepthTarget();
    void ImportCapture();
    void ExportCapture();
    void RemoveCapture();
    void RefreshSessions(Debugger::u64 selected = 0);

    void OnBreakPointHit(Pica::DebugContext::Event event, const void* data) override;
    void OnResumed() override;

signals:
    void SetStartTracingButtonEnabled(bool enable);
    void SetStopTracingButtonEnabled(bool enable);
    void SetAbortTracingButtonEnabled(bool enable);

private:
    void OpenTarget(bool depth);
    void SelectEvent(int event);
    void UpdateOutput(Debugger::u32 sequence);
    void UpdateOutputCaptureState();

    Core::System& system;
    std::shared_ptr<Debugger::RenderSessionManager> render_sessions;
    QWidget* recording_controls;
    QLineEdit* timeline_filter;
    QSpinBox* frame_limit;
    QCheckBox* follow_live;
    QCheckBox* freeze_timeline;
    QTreeWidget* timeline;
    QSlider* event_slider;
    QLabel* event_position;
    QLabel* timeline_details;
    ScaledPixmapLabel* output_preview;
    QLabel* output_status;
    QPushButton* open_color_target;
    QPushButton* open_depth_target;
    QComboBox* session_selector;
    QPushButton* remove_capture;
    QPushButton* load_older;
    std::vector<Debugger::TimelineEntry> displayed_entries;
    std::vector<QTreeWidgetItem*> displayed_draw_items;
    std::optional<Debugger::RenderTarget> selected_target;
    std::optional<Debugger::u32> displayed_output_sequence;
    Debugger::TimelineStatus displayed_status{};
    Debugger::u64 displayed_session_id{};
    bool have_displayed_status{};
    Debugger::u32 displayed_limit{128};
};
