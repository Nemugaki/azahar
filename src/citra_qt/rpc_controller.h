// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <QObject>
#include "core/rpc/packet.h"

namespace Core {
class System;
}

class GMainWindow;
class QLabel;

/// Marshals RPC frontend operations from the server thread onto Qt's main thread.
class RPCController final : public QObject {
public:
    RPCController(GMainWindow& main_window, Core::System& system);
    ~RPCController() override;

    Core::RPC::EmulationControlReply HandleEmulationControl(Core::RPC::EmulationControl operation,
                                                            const std::string& path);

private:
    struct Request {
        Core::RPC::EmulationControl operation;
        std::string path;
        Core::RPC::EmulationControlReply reply{Core::RPC::EmulationResult::Failed,
                                               Core::RPC::EmulationState::Stopped};
        bool done{};
    };

    Core::RPC::EmulationControlReply Execute(Core::RPC::EmulationControl operation,
                                             const std::string& path);
    Core::RPC::EmulationState State() const;
    void DrainRequests();
    void UpdateIndicator();

    GMainWindow& main_window;
    Core::System& system;
    QLabel* indicator{};
    std::mutex mutex;
    std::condition_variable request_completed;
    std::deque<std::shared_ptr<Request>> requests;
    bool stopping{};
    bool listening{};
    u64 request_count{};
    u32 active_client_count{};
};
