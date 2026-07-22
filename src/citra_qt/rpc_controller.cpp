// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QLabel>
#include <QStatusBar>
#include <QThread>
#include "citra_qt/bootmanager.h"
#include "citra_qt/citra_qt.h"
#include "citra_qt/rpc_controller.h"
#include "core/core.h"
#include "core/rpc/udp_server.h"

RPCController::RPCController(GMainWindow& main_window_, Core::System& system_)
    : main_window{main_window_}, system{system_}, indicator{new QLabel(&main_window)} {
    indicator->setFrameStyle(QFrame::NoFrame);
    indicator->setContentsMargins(4, 0, 4, 0);
    indicator->setToolTip(
        tr("Agent RPC on 127.0.0.1:%1 (loopback only)").arg(Core::RPC::GetRPCPort()));
    main_window.statusBar()->addPermanentWidget(indicator);
    UpdateIndicator();

    system.StartRPCServer([this](Core::RPC::EmulationControl operation, const std::string& path) {
        return HandleEmulationControl(operation, path);
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
    indicator->setText(tr("RPC: listening"));
    indicator->setToolTip(
        tr("Agent RPC on 127.0.0.1:%1 (loopback only)\n%2 lifecycle requests handled")
            .arg(Core::RPC::GetRPCPort())
            .arg(request_count));
}
