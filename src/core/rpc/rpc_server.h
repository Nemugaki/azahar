// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <condition_variable>
#include <memory>
#include <span>
#include <vector>
#include "common/polyfill_thread.h"
#include "common/threadsafe_queue.h"
#include "core/rpc/packet.h"

namespace Core {
class System;
}

namespace Core::RPC {

class Packet;
struct PacketHeader;

class RPCServer {
public:
    RPCServer(Core::System& system, EmulationControlHandler emulation_control_handler);
    ~RPCServer();

    void QueueRequest(std::unique_ptr<RPC::Packet> request);

private:
    void HandleReadMemory(Packet& packet, u32 address, u32 data_size);
    void HandleWriteMemory(Packet& packet, u32 address, std::span<const u8> data);
    void HandleProcessList(Packet& packet, u32 start_index, u32 max_amount);
    void HandleSetGetProcess(Packet& packet, u32 operation, u32 process_id);
    void HandleCapabilities(Packet& packet);
    void HandleEmulationControl(Packet& packet, EmulationControl operation,
                                const std::string& path);
    void HandlePicaSnapshot(Packet& packet, PicaSnapshotOperation operation, u32 generation,
                            u32 offset, u32 size);
    void HandlePicaBreakpoint(Packet& packet, PicaBreakpointOperation operation, u32 event,
                              u32 enabled);
    void HandlePicaTrace(Packet& packet, PicaTraceOperation operation, u32 generation, u32 offset,
                         u32 size);
    bool ValidatePacket(const PacketHeader& packet_header);
    void HandleSingleRequest(std::unique_ptr<Packet> request);
    void HandleRequestsLoop(std::stop_token stop_token);

private:
    Core::System& system;
    Common::SPSCQueue<std::unique_ptr<Packet>, true> request_queue;
    std::jthread request_handler_thread;
    EmulationControlHandler emulation_control_handler;
    u32 selected_pid = 0xFFFFFFFF;
    std::vector<u8> pica_trace_data;
    u32 pica_trace_generation = 0;
    bool pica_trace_owned = false;
};

} // namespace Core::RPC
