// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include "common/common_types.h"
#include "core/rpc/packet.h"

namespace Core::RPC {

class Packet;

u16 GetRPCPort();

class UDPServer {
public:
    UDPServer(std::function<void(std::unique_ptr<Packet>)> new_request_callback,
              ClientCountHandler client_count_handler);
    ~UDPServer();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::RPC
