// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <vector>
#include "citra_qt/debugger/graphics/graphics_breakpoint_observer.h"

namespace Core {
class System;
}

class EmuThread;
class QLabel;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

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

    void OnBreakPointHit(Pica::DebugContext::Event event, const void* data) override;
    void OnResumed() override;

signals:
    void SetStartTracingButtonEnabled(bool enable);
    void SetStopTracingButtonEnabled(bool enable);
    void SetAbortTracingButtonEnabled(bool enable);

private:
    Core::System& system;
    QWidget* recording_controls;
    QLineEdit* timeline_filter;
    QSpinBox* frame_limit;
    QCheckBox* follow_live;
    QCheckBox* freeze_timeline;
    QTreeWidget* timeline;
    QLabel* timeline_details;
    QPushButton* open_color_target;
    QPushButton* open_depth_target;
    std::vector<Pica::DebugContext::TimelineEntry> displayed_entries;
    std::optional<Pica::DebugContext::RenderTargetInfo> selected_target;
};
