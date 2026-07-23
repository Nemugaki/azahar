// Copyright 2015 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version

#pragma once

#include "citra_qt/debugger/graphics/graphics_breakpoint_observer.h"

namespace Core {
class System;
}

namespace Debugger {
class CaptureViewerWidget;
}

class EmuThread;
class QWidget;

class GraphicsTracingWidget final : public BreakPointObserverDock {
    Q_OBJECT

public:
    explicit GraphicsTracingWidget(Core::System& system,
                                   std::shared_ptr<Pica::DebugContext> debug_context,
                                   QWidget* parent = nullptr);

    void OnEmulationStarting(EmuThread*);
    void OnEmulationStopping();

private:
    void StartRecording();
    void StopRecording();
    void AbortRecording();
    void OpenTarget(bool depth);
    void OnBreakPointHit(Pica::DebugContext::Event, const void*) override;
    void OnResumed() override;

signals:
    void SetStartTracingButtonEnabled(bool);
    void SetStopTracingButtonEnabled(bool);
    void SetAbortTracingButtonEnabled(bool);

private:
    Core::System& system;
    QWidget* recording_controls{};
    Debugger::CaptureViewerWidget* capture_viewer{};
};
