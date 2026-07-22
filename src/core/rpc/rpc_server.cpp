// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/rpc/packet.h"
#include "core/rpc/rpc_server.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"

namespace Core::RPC {

RPCServer::RPCServer(Core::System& system_, EmulationControlHandler emulation_control_handler_)
    : system{system_}, emulation_control_handler{std::move(emulation_control_handler_)} {
    LOG_INFO(RPC_Server, "Starting RPC server.");
    request_handler_thread =
        std::jthread([this](std::stop_token stop_token) { HandleRequestsLoop(stop_token); });
}

RPCServer::~RPCServer() {
    request_handler_thread.request_stop();
    request_handler_thread.join();
    if (pica_trace_owned && Pica::DebugUtils::IsPicaTracing()) {
        Pica::DebugUtils::FinishPicaTracing();
    }
}

void RPCServer::HandleReadMemory(Packet& packet, u32 address, u32 data_size) {
    if (!system.IsPoweredOn() || data_size > MAX_READ_SIZE) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    u32 read_size = data_size;

    // Note: Memory read occurs asynchronously from the state of the emulator
    if (selected_pid == 0xFFFFFFFF) {
        LOG_ERROR(RPC_Server, "No target process selected, memory access may be invalid.");
        system.Memory().ReadBlock(address, packet.GetPacketData().data(), data_size);
    } else {
        auto process = system.Kernel().GetProcessById(selected_pid);
        if (process) {
            system.Memory().ReadBlock(*process, address, packet.GetPacketData().data(), data_size);
        } else {
            LOG_ERROR(RPC_Server, "Selected process does not exist.");
            read_size = 0;
        }
    }

    packet.SetPacketDataSize(read_size);
    packet.SendReply();
}

void RPCServer::HandleWriteMemory(Packet& packet, u32 address, std::span<const u8> data) {
    if (!system.IsPoweredOn()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }

    // Only allow writing to certain memory regions
    if ((address >= Memory::PROCESS_IMAGE_VADDR && address <= Memory::PROCESS_IMAGE_VADDR_END) ||
        (address >= Memory::HEAP_VADDR && address <= Memory::HEAP_VADDR_END) ||
        (address >= Memory::LINEAR_HEAP_VADDR && address <= Memory::LINEAR_HEAP_VADDR_END) ||
        (address >= Memory::N3DS_EXTRA_RAM_VADDR && address <= Memory::N3DS_EXTRA_RAM_VADDR_END)) {
        // Note: Memory write occurs asynchronously from the state of the emulator
        if (selected_pid == 0xFFFFFFFF) {
            LOG_ERROR(RPC_Server, "No target process selected, memory access may be invalid.");
            system.Memory().WriteBlock(address, data.data(), data.size());
        } else {
            auto process = system.Kernel().GetProcessById(selected_pid);
            if (process) {
                system.Memory().WriteBlock(*process, address, data.data(), data.size());
            } else {
                LOG_ERROR(RPC_Server, "Selected process does not exist.");
            }
        }

        // If the memory happens to be executable code, make sure the changes become visible

        // Is current core correct here?
        system.InvalidateCacheRange(address, data.size());
    }
    packet.SetPacketDataSize(0);
    packet.SendReply();
}

void RPCServer::HandleProcessList(Packet& packet, u32 start_index, u32 max_amount) {
    if (!system.IsPoweredOn()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }

    const auto process_list = system.Kernel().GetProcessList();
    const u32 start = std::min(start_index, static_cast<u32>(process_list.size()));
    const u32 end = std::min(start + max_amount, static_cast<u32>(process_list.size()));
    const u32 count = std::min(end - start, MAX_PROCESSES_IN_LIST);

    u8* out_data = packet.GetPacketData().data();
    u32 written_bytes = 0;

    memcpy(out_data + written_bytes, &count, sizeof(count));
    written_bytes += sizeof(count);

    for (u32 i = start; i < start + count; i++) {
        ProcessInfo info{};
        info.process_id = process_list[i]->process_id;
        info.title_id = process_list[i]->codeset->program_id;
        memcpy(info.process_name.data(), process_list[i]->codeset->name.data(),
               std::min(process_list[i]->codeset->name.size(), info.process_name.size()));

        memcpy(out_data + written_bytes, &info, sizeof(ProcessInfo));
        written_bytes += sizeof(ProcessInfo);
    }

    packet.SetPacketDataSize(written_bytes);
    packet.SendReply();
}

void RPCServer::HandleSetGetProcess(Packet& packet, u32 operation, u32 process_id) {
    u8* out_data = packet.GetPacketData().data();
    u32 written_bytes = 0;

    if (operation == 0) {
        // Get
        memcpy(out_data + written_bytes, &selected_pid, sizeof(selected_pid));
        written_bytes += sizeof(selected_pid);
    } else {
        // Set
        selected_pid = process_id;
    }

    packet.SetPacketDataSize(written_bytes);
    packet.SendReply();
}

void RPCServer::HandleCapabilities(Packet& packet) {
    const std::array response{CURRENT_VERSION,
                              CAPABILITY_EMULATION_CONTROL | CAPABILITY_PICA_SNAPSHOT |
                                  CAPABILITY_PICA_BREAKPOINT | CAPABILITY_PICA_TRACE};
    std::memcpy(packet.GetPacketData().data(), response.data(), sizeof(response));
    packet.SetPacketDataSize(sizeof(response));
    packet.SendReply();
}

void RPCServer::HandlePicaBreakpoint(Packet& packet, PicaBreakpointOperation operation, u32 event,
                                     u32 enabled) {
    const auto context = Pica::g_debug_context;
    if (!context) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    if (operation == PicaBreakpointOperation::Set &&
        event < static_cast<u32>(Pica::DebugContext::Event::NumEvents)) {
        context->SetBreakpoint(static_cast<Pica::DebugContext::Event>(event), enabled != 0);
    } else if (operation == PicaBreakpointOperation::Resume) {
        context->Resume();
    } else if (operation == PicaBreakpointOperation::Clear) {
        context->ClearBreakpoints();
    }
    const auto state = context->GetBreakpointState();
    const PicaBreakpointReply reply{state.enabled_mask, static_cast<u32>(state.active),
                                    state.at_breakpoint};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandlePicaTrace(Packet& packet, PicaTraceOperation operation, u32 generation,
                                u32 offset, u32 size) {
    if (operation == PicaTraceOperation::Start) {
        if (!Pica::DebugUtils::IsPicaTracing()) {
            Pica::DebugUtils::StartPicaTracing();
            pica_trace_owned = true;
        }
    } else if (operation == PicaTraceOperation::Stop) {
        if (pica_trace_owned && Pica::DebugUtils::IsPicaTracing()) {
            if (auto trace = Pica::DebugUtils::FinishPicaTracing()) {
                pica_trace_data.resize(trace->writes.size() *
                                       sizeof(Pica::DebugUtils::PicaTrace::Write));
                if (!pica_trace_data.empty()) {
                    std::memcpy(pica_trace_data.data(), trace->writes.data(),
                                pica_trace_data.size());
                }
                ++pica_trace_generation;
            }
            pica_trace_owned = false;
        }
    } else if (operation == PicaTraceOperation::Read) {
        if (generation == pica_trace_generation && offset <= pica_trace_data.size()) {
            const auto available = static_cast<u32>(pica_trace_data.size() - offset);
            const auto read_size = std::min(size, available);
            std::memcpy(packet.GetPacketData().data(), pica_trace_data.data() + offset, read_size);
            packet.SetPacketDataSize(read_size);
        } else {
            packet.SetPacketDataSize(0);
        }
        packet.SendReply();
        return;
    } else if (operation == PicaTraceOperation::Clear) {
        pica_trace_data.clear();
    }

    const PicaTraceReply reply{Pica::DebugUtils::IsPicaTracing(), pica_trace_generation,
                               static_cast<u32>(pica_trace_data.size())};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandlePicaSnapshot(Packet& packet, PicaSnapshotOperation operation, u32 generation,
                                   u32 offset, u32 size) {
    if (!system.IsPoweredOn()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }

    auto& pica = system.GPU().PicaCore();
    switch (operation) {
    case PicaSnapshotOperation::Arm:
        pica.RequestSnapshot();
        [[fallthrough]];
    case PicaSnapshotOperation::Status: {
        const auto info = pica.GetSnapshotInfo();
        std::memcpy(packet.GetPacketData().data(), std::addressof(info), sizeof(info));
        packet.SetPacketDataSize(sizeof(info));
        break;
    }
    case PicaSnapshotOperation::Read:
        packet.SetPacketDataSize(
            pica.ReadSnapshot(generation, offset, packet.GetPacketData().first(size)));
        break;
    case PicaSnapshotOperation::Clear:
        pica.ClearSnapshot();
        packet.SetPacketDataSize(0);
        break;
    default:
        packet.SetPacketDataSize(0);
        break;
    }
    packet.SendReply();
}

void RPCServer::HandleEmulationControl(Packet& packet, EmulationControl operation,
                                       const std::string& path) {
    EmulationControlReply reply{EmulationResult::Unsupported, EmulationState::Stopped};
    if (emulation_control_handler) {
        reply = emulation_control_handler(operation, path);
    }
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

bool RPCServer::ValidatePacket(const PacketHeader& packet_header) {
    if (packet_header.version <= CURRENT_VERSION) {
        switch (packet_header.packet_type) {
        case PacketType::ReadMemory:
        case PacketType::WriteMemory:
        case PacketType::ProcessList:
        case PacketType::SetGetProcess:
            if (packet_header.packet_size >= (sizeof(u32) * 2)) {
                return true;
            }
            break;
        case PacketType::Capabilities:
            return packet_header.packet_size == 0;
        case PacketType::EmulationControl:
            return packet_header.packet_size >= sizeof(u32);
        case PacketType::PicaSnapshot:
        case PacketType::PicaBreakpoint:
        case PacketType::PicaTrace:
            return packet_header.packet_size >= sizeof(u32);
        default:
            break;
        }
    }
    return false;
}

void RPCServer::HandleSingleRequest(std::unique_ptr<Packet> request_packet) {
    bool success = false;
    const auto packet_data = request_packet->GetPacketData();

    if (ValidatePacket(request_packet->GetHeader())) {
        // Legacy request types use two arguments.
        u32 arg1 = 0;
        u32 arg2 = 0;
        if (request_packet->GetPacketDataSize() >= sizeof(arg1)) {
            std::memcpy(&arg1, packet_data.data(), sizeof(arg1));
        }
        if (request_packet->GetPacketDataSize() >= sizeof(arg1) + sizeof(arg2)) {
            std::memcpy(&arg2, packet_data.data() + sizeof(arg1), sizeof(arg2));
        }

        switch (request_packet->GetPacketType()) {
        case PacketType::ReadMemory:
            if (arg2 > 0 && arg2 <= MAX_READ_SIZE) {
                HandleReadMemory(*request_packet, arg1, arg2);
                success = true;
            }
            break;
        case PacketType::WriteMemory:
            if (arg2 > 0 && arg2 <= MAX_PACKET_DATA_SIZE - (sizeof(u32) * 2)) {
                const auto data = packet_data.subspan(sizeof(u32) * 2, arg2);
                HandleWriteMemory(*request_packet, arg1, data);
                success = true;
            }
            break;
        case PacketType::ProcessList:
            HandleProcessList(*request_packet, arg1, arg2);
            success = true;
            break;
        case PacketType::SetGetProcess:
            HandleSetGetProcess(*request_packet, arg1, arg2);
            success = true;
            break;
        case PacketType::Capabilities:
            HandleCapabilities(*request_packet);
            success = true;
            break;
        case PacketType::EmulationControl: {
            const auto path_size = request_packet->GetPacketDataSize() - sizeof(u32);
            const std::string path{reinterpret_cast<const char*>(packet_data.data() + sizeof(u32)),
                                   path_size};
            HandleEmulationControl(*request_packet, static_cast<EmulationControl>(arg1), path);
            success = true;
            break;
        }
        case PacketType::PicaSnapshot: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (arg4 <= MAX_PACKET_DATA_SIZE) {
                HandlePicaSnapshot(*request_packet, static_cast<PicaSnapshotOperation>(arg1), arg2,
                                   arg3, arg4);
                success = true;
            }
            break;
        }
        case PacketType::PicaBreakpoint: {
            u32 arg3 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            HandlePicaBreakpoint(*request_packet, static_cast<PicaBreakpointOperation>(arg1), arg2,
                                 arg3);
            success = true;
            break;
        }
        case PacketType::PicaTrace: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (arg4 <= MAX_PACKET_DATA_SIZE) {
                HandlePicaTrace(*request_packet, static_cast<PicaTraceOperation>(arg1), arg2, arg3,
                                arg4);
                success = true;
            }
            break;
        }
        default:
            break;
        }
    }

    if (!success) {
        // Send an empty reply, so as not to hang the client
        request_packet->SetPacketDataSize(0);
        request_packet->SendReply();
    }
}

void RPCServer::HandleRequestsLoop(std::stop_token stop_token) {
    std::unique_ptr<RPC::Packet> request_packet;

    LOG_INFO(RPC_Server, "Request handler started.");

    while ((request_packet = request_queue.PopWait(stop_token))) {
        HandleSingleRequest(std::move(request_packet));
    }
}

void RPCServer::QueueRequest(std::unique_ptr<RPC::Packet> request) {
    request_queue.Push(std::move(request));
}

}; // namespace Core::RPC
