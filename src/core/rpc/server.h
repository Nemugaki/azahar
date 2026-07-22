// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "core/rpc/rpc_server.h"

namespace Core {
class System;
}

namespace Core::RPC {

class UDPServer;
class Packet;

class Server {
public:
    Server(Core::System& system_, EmulationControlHandler emulation_control_handler);
    ~Server();

    bool IsListening() const {
        return udp_server != nullptr;
    }

    void NewRequestCallback(std::unique_ptr<Packet> new_request);

private:
    RPCServer rpc_server;
    std::unique_ptr<UDPServer> udp_server;
};

} // namespace Core::RPC
