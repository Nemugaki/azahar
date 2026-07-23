// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
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
                              u32 argument, u32 value, u32 mask);
    void HandlePicaTimeline(Packet& packet, PicaTimelineOperation operation, u32 start, u32 count,
                            u32 kind, u32 required_changes, u32 target_address, u32 shader_entry,
                            u32 frame);
    void HandlePicaRenderTarget(Packet& packet);
    void HandlePicaTrace(Packet& packet, PicaTraceOperation operation, u32 generation, u32 start,
                         u32 count, u32 register_id);
    void HandleCPURegisters(Packet& packet, u32 core, u32 bank, u32 start, u32 count);
    void HandleGXCommandTrace(Packet& packet, GXCommandTraceOperation operation, u32 start,
                              u32 count, u32 command_id);
    void HandlePicaShader(Packet& packet, PicaShaderOperation operation, u32 generation, u32 start,
                          u32 count, u32 instruction_offset);
    void HandleDebugState(Packet& packet, DebugStateOperation operation, u32 after_generation,
                          u32 timeout_ms, u32 reason_mask);
    void HandleDebugCapture(Packet& packet, DebugCaptureOperation operation, u32 id, u32 argument,
                            u32 start, u32 count);
    void HandleRenderSession(Packet& packet, RenderSessionOperation operation, u64 id, u32 start,
                             u32 count, const std::string& path);
    void HandleRenderOutput(Packet& packet, RenderOutputOperation operation, u64 session_id,
                            u32 sequence, u32 offset, u32 count);
    void HandleRenderDebug(Packet& packet, RenderDebugOperation operation,
                           std::span<const u8> request);
    u32 GetEnabledCapabilities() const;
    bool IsPacketTypeEnabled(PacketType packet_type) const;
    bool IsEmulationControlEnabled(EmulationControl operation) const;
    bool ValidatePacket(const PacketHeader& packet_header) const;
    void SendError(Packet& packet, Error error) const;
    void HandleSingleRequest(std::unique_ptr<Packet> request);
    void HandleRequestsLoop(std::stop_token stop_token);
    void HandleWaitRequestsLoop(std::stop_token stop_token);

private:
    Core::System& system;
    Common::SPSCQueue<std::unique_ptr<Packet>, true> request_queue;
    Common::SPSCQueue<std::unique_ptr<Packet>, true> wait_request_queue;
    std::jthread request_handler_thread;
    std::jthread wait_request_handler_thread;
    EmulationControlHandler emulation_control_handler;
    u32 selected_pid = 0xFFFFFFFF;
    std::vector<u8> pica_trace_data;
    u32 pica_trace_generation = 0;
    bool pica_trace_owned = false;
    bool pica_trace_truncated = false;
    std::vector<u8> pica_shader_dump;
    std::vector<PicaShaderCycle> pica_shader_cycles;
    u32 pica_shader_generation = 0;
    bool pica_shader_vertex_input_valid = false;
    std::mutex render_capture_mutex;
};

} // namespace Core::RPC
