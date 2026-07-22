// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include "common/common_types.h"

namespace Core::RPC {

class Packet;

u16 GetRPCPort();

class UDPServer {
public:
    explicit UDPServer(std::function<void(std::unique_ptr<Packet>)> new_request_callback);
    ~UDPServer();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core::RPC
