// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <charconv>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QStatusBar>
#include <QThread>
#include "citra_qt/bootmanager.h"
#include "citra_qt/citra_qt.h"
#include "citra_qt/rpc_controller.h"
#include "citra_qt/uisettings.h"
#include "core/core.h"
#include "core/savestate.h"
#include "core/rpc/udp_server.h"

RPCController::RPCController(GMainWindow& main_window_, Core::System& system_)
    : main_window{main_window_}, system{system_}, indicator{new QLabel(&main_window)} {
    indicator->setFrameStyle(QFrame::NoFrame);
    indicator->setContentsMargins(4, 0, 4, 0);
    indicator->setToolTip(
        tr("RPC server on 127.0.0.1:%1 (loopback only)").arg(Core::RPC::GetRPCPort()));
    main_window.statusBar()->addPermanentWidget(indicator);
    UpdateIndicator();

    system.StartRPCServer(
        [this](Core::RPC::EmulationControl operation, const std::string& path) {
            return HandleEmulationControl(operation, path);
        },
        [this](u32 count) {
            QMetaObject::invokeMethod(
                this,
                [this, count] {
                    active_client_count = count;
                    UpdateIndicator();
                },
                Qt::QueuedConnection);
        });
}

RPCController::~RPCController() {
    {
        std::lock_guard lock{mutex};
        stopping = true;
        for (const auto& request : requests) {
            request->done = true;
        }
        requests.clear();
    }
    request_completed.notify_all();
    system.StopRPCServer();
    delete indicator;
}

Core::RPC::EmulationControlReply RPCController::HandleEmulationControl(
    Core::RPC::EmulationControl operation, const std::string& path) {
    if (QThread::currentThread() == thread()) {
        return Execute(operation, path);
    }

    auto request = std::make_shared<Request>();
    request->operation = operation;
    request->path = path;
    std::unique_lock lock{mutex};
    if (stopping) {
        return request->reply;
    }
    requests.push_back(request);
    QMetaObject::invokeMethod(this, [this] { DrainRequests(); }, Qt::QueuedConnection);
    request_completed.wait(lock, [this, &request] { return stopping || request->done; });
    return request->reply;
}

Core::RPC::EmulationState RPCController::State() const {
    if (!main_window.emulation_running || !main_window.emu_thread) {
        return Core::RPC::EmulationState::Stopped;
    }
    if (!main_window.emu_thread->IsRunning() || system.frame_limiter.IsFrameAdvancing()) {
        return Core::RPC::EmulationState::Paused;
    }
    return Core::RPC::EmulationState::Running;
}

Core::RPC::EmulationControlReply RPCController::Execute(Core::RPC::EmulationControl operation,
                                                        const std::string& path) {
    Core::RPC::EmulationControlReply reply{Core::RPC::EmulationResult::Success, State()};
    switch (operation) {
    case Core::RPC::EmulationControl::Status:
        break;
    case Core::RPC::EmulationControl::Run:
        if (path.empty()) {
            reply.result = Core::RPC::EmulationResult::InvalidArgument;
        } else {
            main_window.BootGame(QString::fromStdString(path));
            if (!main_window.emulation_running) {
                reply.result = Core::RPC::EmulationResult::Failed;
            }
        }
        break;
    case Core::RPC::EmulationControl::Pause:
        if (State() != Core::RPC::EmulationState::Running) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            main_window.OnPauseGame();
        }
        break;
    case Core::RPC::EmulationControl::Resume:
        if (State() != Core::RPC::EmulationState::Paused) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            main_window.OnResumeGame(false);
        }
        break;
    case Core::RPC::EmulationControl::Stop:
        if (State() == Core::RPC::EmulationState::Stopped) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            main_window.OnStopGame();
        }
        break;
    case Core::RPC::EmulationControl::Restart:
        if (State() == Core::RPC::EmulationState::Stopped) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            const auto restart_path = main_window.game_path;
            main_window.BootGame(restart_path);
            if (!main_window.emulation_running) {
                reply.result = Core::RPC::EmulationResult::Failed;
            }
        }
        break;
    case Core::RPC::EmulationControl::DebugPause:
        if (State() == Core::RPC::EmulationState::Stopped || !main_window.emu_thread) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            main_window.emu_thread->SetRunning(false);
        }
        break;
    case Core::RPC::EmulationControl::DebugResume:
        if (!main_window.emu_thread || main_window.emu_thread->IsRunning()) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else {
            main_window.emu_thread->SetRunning(true);
        }
        break;
    case Core::RPC::EmulationControl::SaveState:
    case Core::RPC::EmulationControl::LoadState: {
        u32 slot{};
        const auto [end, error] = std::from_chars(path.data(), path.data() + path.size(), slot);
        if (State() == Core::RPC::EmulationState::Stopped) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else if (error != std::errc{} || end != path.data() + path.size() ||
                   slot >= Core::SaveStateSlotCount) {
            reply.result = Core::RPC::EmulationResult::InvalidArgument;
        } else {
            const auto signal = operation == Core::RPC::EmulationControl::SaveState
                                    ? Core::System::Signal::Save
                                    : Core::System::Signal::Load;
            if (!system.SendSignal(signal, slot)) {
                reply.result = Core::RPC::EmulationResult::Failed;
            } else {
                system.frame_limiter.AdvanceFrame();
            }
        }
        break;
    }
    case Core::RPC::EmulationControl::Screenshot: {
        const QFileInfo output{QString::fromStdString(path)};
        if (State() != Core::RPC::EmulationState::Running) {
            reply.result = Core::RPC::EmulationResult::InvalidState;
        } else if (!output.isAbsolute() || output.fileName().isEmpty()) {
            reply.result = Core::RPC::EmulationResult::InvalidArgument;
        } else if (output.exists() || !QDir{}.mkpath(output.absolutePath())) {
            reply.result = Core::RPC::EmulationResult::Failed;
        } else {
            main_window.OnPauseGame();
            auto* const screenshot_window = main_window.secondary_window->HasFocus()
                                                ? main_window.secondary_window
                                                : main_window.render_window;
            screenshot_window->CaptureScreenshot(
                UISettings::values.screenshot_resolution_factor.GetValue(), output.filePath());
            main_window.OnResumeGame(false);
        }
        break;
    }
    default:
        reply.result = Core::RPC::EmulationResult::Unsupported;
        break;
    }
    reply.state = State();
    UpdateIndicator();
    return reply;
}

void RPCController::DrainRequests() {
    while (true) {
        std::shared_ptr<Request> request;
        {
            std::lock_guard lock{mutex};
            if (requests.empty() || stopping) {
                return;
            }
            request = std::move(requests.front());
            requests.pop_front();
        }

        ++request_count;
        const auto reply = Execute(request->operation, request->path);
        {
            std::lock_guard lock{mutex};
            request->reply = reply;
            request->done = true;
        }
        request_completed.notify_all();
    }
}

void RPCController::UpdateIndicator() {
    indicator->setText(tr("RPC: Listening at %1 | %2 active")
                           .arg(Core::RPC::GetRPCPort())
                           .arg(active_client_count));
    indicator->setToolTip(
        tr("RPC server on 127.0.0.1:%1 (loopback only)\n%2 clients active in the last 10 seconds\n%3 frontend requests handled")
            .arg(Core::RPC::GetRPCPort())
            .arg(active_client_count)
            .arg(request_count));
}
