// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/rpc/packet.h"
#include "core/rpc/rpc_server.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/pica/pica_core.h"

namespace Core::RPC {

namespace {

bool IsInsideRegion(u32 address, std::size_t size, u32 begin, u32 last) {
    const u64 end = static_cast<u64>(address) + size;
    return size != 0 && address >= begin && end <= static_cast<u64>(last) + 1;
}

bool IsWritableMemoryRange(u32 address, std::size_t size) {
    return IsInsideRegion(address, size, Memory::PROCESS_IMAGE_VADDR,
                          Memory::PROCESS_IMAGE_VADDR_END) ||
           IsInsideRegion(address, size, Memory::HEAP_VADDR, Memory::HEAP_VADDR_END) ||
           IsInsideRegion(address, size, Memory::LINEAR_HEAP_VADDR,
                          Memory::LINEAR_HEAP_VADDR_END) ||
           IsInsideRegion(address, size, Memory::N3DS_EXTRA_RAM_VADDR,
                          Memory::N3DS_EXTRA_RAM_VADDR_END);
}

} // namespace

RPCServer::RPCServer(Core::System& system_, EmulationControlHandler emulation_control_handler_)
    : system{system_}, emulation_control_handler{std::move(emulation_control_handler_)} {
    LOG_INFO(RPC_Server, "Starting RPC server.");
    request_handler_thread =
        std::jthread([this](std::stop_token stop_token) { HandleRequestsLoop(stop_token); });
    wait_request_handler_thread =
        std::jthread([this](std::stop_token stop_token) { HandleWaitRequestsLoop(stop_token); });
}

RPCServer::~RPCServer() {
    request_handler_thread.request_stop();
    wait_request_handler_thread.request_stop();
    request_handler_thread.join();
    wait_request_handler_thread.join();
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
        SendError(packet, Error::InvalidState);
        return;
    }

    if (IsWritableMemoryRange(address, data.size())) {
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
                SendError(packet, Error::NotFound);
                return;
            }
        }

        // If the memory happens to be executable code, make sure the changes become visible

        // Is current core correct here?
        system.InvalidateCacheRange(address, data.size());
    } else {
        SendError(packet, Error::InvalidArgument);
        return;
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
    const std::array response{CURRENT_VERSION, GetEnabledCapabilities()};
    std::memcpy(packet.GetPacketData().data(), response.data(), sizeof(response));
    packet.SetPacketDataSize(sizeof(response));
    packet.SendReply();
}

u32 RPCServer::GetEnabledCapabilities() const {
    if (!Settings::values.enable_rpc_server.GetValue()) {
        return 0;
    }

    u32 capabilities = CAPABILITY_ERROR_REPLIES | CAPABILITY_REQUEST_DEDUPLICATION;
    if (Settings::values.rpc_allow_memory.GetValue()) {
        capabilities |= CAPABILITY_MEMORY_ACCESS;
    }
    if (Settings::values.rpc_allow_memory_write.GetValue()) {
        capabilities |= CAPABILITY_MEMORY_WRITE;
    }
    if (Settings::values.rpc_allow_emulation_control.GetValue()) {
        capabilities |= CAPABILITY_EMULATION_CONTROL;
    }
    if (Settings::values.rpc_allow_cpu_registers.GetValue()) {
        capabilities |= CAPABILITY_CPU_REGISTERS;
    }
    if (Settings::values.rpc_allow_graphics_debugger.GetValue()) {
        capabilities |= CAPABILITY_GX_COMMAND_TRACE;
    }
    if (Settings::values.rpc_allow_save_states.GetValue()) {
        capabilities |= CAPABILITY_SAVE_STATES;
    }
    if (Settings::values.rpc_allow_screenshots.GetValue()) {
        capabilities |= CAPABILITY_SCREENSHOTS;
    }
    if (Settings::values.pica_debugging.GetValue()) {
        if (Settings::values.rpc_allow_pica_snapshot.GetValue()) {
            capabilities |= CAPABILITY_PICA_SNAPSHOT | CAPABILITY_PICA_RENDER_TARGET;
        }
        if (Settings::values.rpc_allow_pica_breakpoints.GetValue()) {
            capabilities |= CAPABILITY_PICA_BREAKPOINT;
        }
        if (Settings::values.rpc_allow_pica_command_list.GetValue()) {
            capabilities |= CAPABILITY_PICA_TRACE | CAPABILITY_PICA_TIMELINE;
        }
        if (Settings::values.rpc_allow_pica_vertex_shader.GetValue()) {
            capabilities |= CAPABILITY_PICA_SHADER;
        }
    }
    if (capabilities & CAPABILITY_EMULATION_CONTROL) {
        capabilities |= CAPABILITY_DEBUG_STATE;
    }
    if ((capabilities & (CAPABILITY_CPU_REGISTERS | CAPABILITY_PICA_SNAPSHOT)) ==
        (CAPABILITY_CPU_REGISTERS | CAPABILITY_PICA_SNAPSHOT)) {
        capabilities |= CAPABILITY_DEBUG_CAPTURE;
    }
    return capabilities;
}

bool RPCServer::IsPacketTypeEnabled(PacketType packet_type) const {
    const u32 capabilities = GetEnabledCapabilities();
    switch (packet_type) {
    case PacketType::Capabilities:
        return true;
    case PacketType::ReadMemory:
    case PacketType::ProcessList:
    case PacketType::SetGetProcess:
        return capabilities & CAPABILITY_MEMORY_ACCESS;
    case PacketType::WriteMemory:
        return capabilities & CAPABILITY_MEMORY_WRITE;
    case PacketType::EmulationControl:
        return capabilities &
               (CAPABILITY_EMULATION_CONTROL | CAPABILITY_SAVE_STATES | CAPABILITY_SCREENSHOTS);
    case PacketType::CPURegisters:
        return capabilities & CAPABILITY_CPU_REGISTERS;
    case PacketType::GXCommandTrace:
        return capabilities & CAPABILITY_GX_COMMAND_TRACE;
    case PacketType::PicaSnapshot:
        return capabilities & CAPABILITY_PICA_SNAPSHOT;
    case PacketType::PicaBreakpoint:
        return capabilities & CAPABILITY_PICA_BREAKPOINT;
    case PacketType::PicaTrace:
        return capabilities & CAPABILITY_PICA_TRACE;
    case PacketType::PicaTimeline:
        return capabilities & CAPABILITY_PICA_TIMELINE;
    case PacketType::PicaRenderTarget:
        return capabilities & CAPABILITY_PICA_RENDER_TARGET;
    case PacketType::PicaShader:
        return capabilities & CAPABILITY_PICA_SHADER;
    case PacketType::DebugState:
        return capabilities & CAPABILITY_DEBUG_STATE;
    case PacketType::DebugCapture:
        return capabilities & CAPABILITY_DEBUG_CAPTURE;
    default:
        return false;
    }
}

bool RPCServer::IsEmulationControlEnabled(EmulationControl operation) const {
    const u32 capabilities = GetEnabledCapabilities();
    switch (operation) {
    case EmulationControl::SaveState:
    case EmulationControl::LoadState:
        return capabilities & CAPABILITY_SAVE_STATES;
    case EmulationControl::Screenshot:
        return capabilities & CAPABILITY_SCREENSHOTS;
    default:
        return capabilities & CAPABILITY_EMULATION_CONTROL;
    }
}

void RPCServer::HandlePicaBreakpoint(Packet& packet, PicaBreakpointOperation operation, u32 event,
                                     u32 argument, u32 value, u32 mask) {
    const auto context = Pica::g_debug_context;
    if (!context) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    if (operation == PicaBreakpointOperation::Set &&
        event < static_cast<u32>(Pica::DebugContext::Event::NumEvents)) {
        context->SetBreakpoint(static_cast<Pica::DebugContext::Event>(event), argument != 0);
    } else if (operation == PicaBreakpointOperation::Resume) {
        context->Resume();
    } else if (operation == PicaBreakpointOperation::Clear) {
        context->ClearBreakpoints();
    } else if ((operation == PicaBreakpointOperation::SetCondition ||
                operation == PicaBreakpointOperation::GetCondition) &&
               event < static_cast<u32>(Pica::DebugContext::Event::NumEvents)) {
        if (operation == PicaBreakpointOperation::SetCondition) {
            if (argument > static_cast<u32>(Pica::DebugContext::ConditionField::FrameIndex)) {
                packet.SetPacketDataSize(0);
                packet.SendReply();
                return;
            }
            context->SetBreakpointCondition(
                static_cast<Pica::DebugContext::Event>(event),
                {static_cast<Pica::DebugContext::ConditionField>(argument), value, mask});
        }
        const auto condition =
            context->GetBreakpointCondition(static_cast<Pica::DebugContext::Event>(event));
        const PicaBreakpointConditionReply reply{static_cast<u32>(condition.field),
                                                  condition.value, condition.mask};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }
    const auto state = context->GetBreakpointState();
    const PicaBreakpointReply reply{state.enabled_mask, static_cast<u32>(state.active),
                                    state.at_breakpoint};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandlePicaTimeline(Packet& packet, PicaTimelineOperation operation, u32 start,
                                   u32 count, u32 kind, u32 required_changes, u32 target_address,
                                   u32 shader_entry, u32 frame) {
    const auto context = Pica::g_debug_context;
    if (!context) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    if (operation == PicaTimelineOperation::Clear) {
        context->ClearTimeline();
    } else if (operation == PicaTimelineOperation::Read) {
        const u32 max_entries = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(PicaTimelineEntry);
        const bool filter_kind = kind <= static_cast<u32>(Pica::DebugContext::TimelineKind::Frame);
        const auto entries = context->GetTimeline(
            start, std::min(count, max_entries),
            static_cast<Pica::DebugContext::TimelineKind>(filter_kind ? kind : 0), filter_kind,
            required_changes, target_address, shader_entry, frame);
        const u32 returned = static_cast<u32>(entries.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 i = 0; i < returned; ++i) {
            const auto& source = entries[i];
            const PicaTimelineEntry target{
                source.sequence,
                static_cast<u32>(source.kind),
                source.frame,
                source.draw,
                source.changed_mask,
                {source.target.color_address, source.target.depth_address, source.target.width,
                 source.target.height, source.target.color_format, source.target.depth_format},
                static_cast<u32>(source.draw_info.mode),
                source.draw_info.vertex_count,
                source.draw_info.topology,
                source.draw_info.vertex_offset,
                source.draw_info.vertex_shader_entry};
            std::memcpy(packet.GetPacketData().data() + sizeof(returned) + i * sizeof(target),
                        &target, sizeof(target));
        }
        packet.SetPacketDataSize(sizeof(returned) + returned * sizeof(PicaTimelineEntry));
        packet.SendReply();
        return;
    }
    const auto status = context->GetTimelineStatus();
    const PicaTimelineStatus reply{status.count, status.oldest_sequence, status.newest_sequence,
                                   status.truncated};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandlePicaRenderTarget(Packet& packet) {
    const auto context = Pica::g_debug_context;
    if (!context) {
        packet.SetPacketDataSize(0);
    } else {
        const auto target = context->GetRenderTargetInfo();
        const PicaRenderTargetReply reply{target.color_address, target.depth_address, target.width,
                                          target.height, target.color_format,
                                          target.depth_format};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
    }
    packet.SendReply();
}

void RPCServer::HandlePicaTrace(Packet& packet, PicaTraceOperation operation, u32 generation,
                                u32 start, u32 count, u32 register_id) {
    if (operation == PicaTraceOperation::Start) {
        if (!Pica::DebugUtils::IsPicaTracing()) {
            Pica::DebugUtils::StartPicaTracing();
            pica_trace_owned = true;
        }
    } else if (operation == PicaTraceOperation::Stop) {
        if (pica_trace_owned && Pica::DebugUtils::IsPicaTracing()) {
            if (auto trace = Pica::DebugUtils::FinishPicaTracing()) {
                pica_trace_truncated = trace->truncated;
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
        if (generation == pica_trace_generation && start <= pica_trace_data.size()) {
            const auto available = static_cast<u32>(pica_trace_data.size() - start);
            const auto read_size = std::min(count, available);
            std::memcpy(packet.GetPacketData().data(), pica_trace_data.data() + start, read_size);
            packet.SetPacketDataSize(read_size);
        } else {
            packet.SetPacketDataSize(0);
        }
        packet.SendReply();
        return;
    } else if (operation == PicaTraceOperation::ReadWrites) {
        using Write = Pica::DebugUtils::PicaTrace::Write;
        const u32 total = static_cast<u32>(pica_trace_data.size() / sizeof(Write));
        const u32 max_writes = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(Write);
        std::vector<Write> result;
        if (generation == pica_trace_generation) {
            for (u32 i = std::min(start, total);
                 i < total && result.size() < std::min(count, max_writes); ++i) {
                Write write;
                std::memcpy(&write, pica_trace_data.data() + i * sizeof(Write), sizeof(write));
                if (register_id == UINT32_MAX || write.cmd_id == register_id) {
                    result.push_back(write);
                }
            }
        }
        const u32 returned = static_cast<u32>(result.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        if (!result.empty()) {
            std::memcpy(packet.GetPacketData().data() + sizeof(returned), result.data(),
                        result.size() * sizeof(result.front()));
        }
        packet.SetPacketDataSize(sizeof(returned) + result.size() * sizeof(result.front()));
        packet.SendReply();
        return;
    } else if (operation == PicaTraceOperation::Clear) {
        pica_trace_data.clear();
        pica_trace_truncated = false;
    }

    const PicaTraceReply reply{Pica::DebugUtils::IsPicaTracing(), pica_trace_generation,
                               static_cast<u32>(pica_trace_data.size()), pica_trace_truncated};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandleCPURegisters(Packet& packet, u32 core, u32 bank, u32 start, u32 count) {
    if (!system.IsPoweredOn() || !system.IsCPUHalted()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }

    if (core >= system.GetNumCores()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    const auto snapshot = system.GetCore(core).GetRegisterSnapshot();
    const u32 bank_size = bank == 0   ? snapshot.core.size()
                          : bank == 1 ? snapshot.vfp.size()
                          : bank == 2 ? 3
                                      : 0;
    const u32 first = std::min(start, bank_size);
    const u32 end = first + std::min({count, static_cast<u32>(MAX_PACKET_DATA_SIZE / sizeof(u32)),
                                      bank_size - first});
    u32 written = 0;
    for (u32 index = first; index < end; ++index) {
        u32 value{};
        if (bank == 0) {
            value = snapshot.core[index];
        } else if (bank == 1) {
            value = snapshot.vfp[index];
        } else if (index == 0) {
            value = snapshot.cpsr;
        } else {
            value = index == 1 ? snapshot.fpscr : snapshot.fpexc;
        }
        std::memcpy(packet.GetPacketData().data() + written, &value, sizeof(value));
        written += sizeof(value);
    }
    packet.SetPacketDataSize(written);
    packet.SendReply();
}

void RPCServer::HandleDebugState(Packet& packet, DebugStateOperation operation,
                                 u32 after_generation, u32 timeout_ms, u32 reason_mask) {
    const auto state = operation == DebugStateOperation::Wait
                           ? system.WaitForDebugState(after_generation, timeout_ms, reason_mask)
                           : system.GetDebugState();
    const DebugStateReply reply{static_cast<u32>(state.reason), state.detail, state.generation};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandleDebugCapture(Packet& packet, DebugCaptureOperation operation, u32 id,
                                   u32 argument, u32 start, u32 count) {
    if (operation == DebugCaptureOperation::Read) {
        packet.SetPacketDataSize(system.ReadDebugCapture(
            id, argument, packet.GetPacketData().first(std::min(count, MAX_PACKET_DATA_SIZE))));
        packet.SendReply();
        return;
    }
    if (operation == DebugCaptureOperation::Diff) {
        const u32 max_entries =
            (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(DebugCaptureDiffReply);
        const auto changes = system.DiffDebugCaptures(id, argument, start,
                                                       std::min(count, max_entries));
        const u32 returned = static_cast<u32>(changes.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 index = 0; index < returned; ++index) {
            const DebugCaptureDiffReply reply{changes[index].offset, changes[index].before,
                                               changes[index].after};
            std::memcpy(packet.GetPacketData().data() + sizeof(returned) + index * sizeof(reply),
                        &reply, sizeof(reply));
        }
        packet.SetPacketDataSize(sizeof(returned) + returned * sizeof(DebugCaptureDiffReply));
        packet.SendReply();
        return;
    }
    if (operation == DebugCaptureOperation::List) {
        const u32 max_entries =
            (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(DebugCaptureReply);
        const auto captures = system.ListDebugCaptures(id, std::min(argument, max_entries));
        const u32 returned = static_cast<u32>(captures.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 index = 0; index < returned; ++index) {
            const auto& info = captures[index];
            const DebugCaptureReply reply{info.id, info.size, static_cast<u32>(info.reason),
                                          info.detail, info.pinned};
            std::memcpy(packet.GetPacketData().data() + sizeof(returned) + index * sizeof(reply),
                        &reply, sizeof(reply));
        }
        packet.SetPacketDataSize(sizeof(returned) + returned * sizeof(DebugCaptureReply));
        packet.SendReply();
        return;
    }
    if (operation == DebugCaptureOperation::Delete) {
        if (!system.DeleteDebugCapture(id)) {
            SendError(packet, Error::NotFound);
            return;
        }
    } else if (operation == DebugCaptureOperation::Pin) {
        if (!system.SetDebugCapturePinned(id, argument != 0)) {
            SendError(packet, Error::NotFound);
            return;
        }
    } else if (operation == DebugCaptureOperation::CacheStatus) {
        const auto info = system.GetDebugCaptureCacheInfo();
        const DebugCaptureCacheReply reply{info.count, info.used, info.limit};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }

    const auto info = operation == DebugCaptureOperation::Create
                          ? system.CreateDebugCapture()
                          : system.GetDebugCaptureInfo(id);
    const DebugCaptureReply reply{info.id, info.size, static_cast<u32>(info.reason), info.detail,
                                  info.pinned};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandleGXCommandTrace(Packet& packet, GXCommandTraceOperation operation, u32 start,
                                     u32 count, u32 command_id) {
    if (!system.IsPoweredOn()) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }
    auto& debugger = system.GPU().Debugger();
    if (operation == GXCommandTraceOperation::Start) {
        debugger.StartCapture();
    } else if (operation == GXCommandTraceOperation::Stop) {
        debugger.StopCapture();
    } else if (operation == GXCommandTraceOperation::Clear) {
        debugger.ClearCapture();
    } else if (operation == GXCommandTraceOperation::Read) {
        constexpr u32 max_commands =
            (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(Service::GSP::Command);
        const auto commands =
            debugger.ReadGXCommands(start, std::min(count, max_commands), command_id);
        const u32 returned = static_cast<u32>(commands.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        if (!commands.empty()) {
            std::memcpy(packet.GetPacketData().data() + sizeof(returned), commands.data(),
                        commands.size() * sizeof(commands.front()));
        }
        packet.SetPacketDataSize(sizeof(returned) + commands.size() * sizeof(commands.front()));
        packet.SendReply();
        return;
    }
    const std::array reply{static_cast<u32>(debugger.IsCapturing()), debugger.GetGXCommandCount()};
    std::memcpy(packet.GetPacketData().data(), reply.data(), sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandlePicaShader(Packet& packet, PicaShaderOperation operation, u32 generation,
                                 u32 start, u32 count, u32 instruction_offset) {
    const auto context = Pica::g_debug_context;
    if (!system.IsPoweredOn() || !context) {
        packet.SetPacketDataSize(0);
        packet.SendReply();
        return;
    }

    if (operation == PicaShaderOperation::Prepare) {
        if (!context->GetBreakpointState().at_breakpoint) {
            packet.SetPacketDataSize(0);
            packet.SendReply();
            return;
        }
        auto& pica = system.GPU().PicaCore();
        const auto vertex_input = context->GetVertexInput();
        pica_shader_vertex_input_valid = vertex_input.has_value();
        Pica::AttributeBuffer input{};
        if (vertex_input) {
            input = *vertex_input;
        }
        auto snapshot = Pica::DebugUtils::CaptureVertexShader(
            pica.regs.internal.vs, pica.vs_setup,
            pica.regs.internal.rasterizer.vs_output_attributes, input);
        pica_shader_dump = std::move(snapshot.binary);
        pica_shader_cycles.clear();
        pica_shader_cycles.reserve(snapshot.cycles.records.size());
        for (u32 cycle = 0; cycle < snapshot.cycles.records.size(); ++cycle) {
            const auto& source = snapshot.cycles.records[cycle];
            PicaShaderCycle target{cycle,
                                   source.instruction_offset,
                                   source.mask & Pica::Shader::DebugDataRecord::NEXT_INSTR
                                       ? source.next_instruction
                                       : UINT32_MAX,
                                   source.mask,
                                   {},
                                   {},
                                   0,
                                   0};
            const std::array vectors{source.src1, source.src2, source.src3, source.dest_in,
                                     source.dest_out};
            constexpr std::array vector_masks{
                Pica::Shader::DebugDataRecord::SRC1, Pica::Shader::DebugDataRecord::SRC2,
                Pica::Shader::DebugDataRecord::SRC3, Pica::Shader::DebugDataRecord::DEST_IN,
                Pica::Shader::DebugDataRecord::DEST_OUT};
            u32 output = 0;
            for (u32 vector_index = 0; vector_index < vectors.size(); ++vector_index) {
                for (u32 component = 0; component < 4; ++component) {
                    if (source.mask & vector_masks[vector_index]) {
                        const float value = vectors[vector_index][component].ToFloat32();
                        std::memcpy(&target.vectors[output], &value, sizeof(value));
                    }
                    ++output;
                }
            }
            if (source.mask & Pica::Shader::DebugDataRecord::ADDR_REG_OUT) {
                target.address_registers = {source.address_registers[0],
                                            source.address_registers[1]};
            }
            if (source.mask & Pica::Shader::DebugDataRecord::CMP_RESULT) {
                target.condition_bits |= static_cast<u32>(source.conditional_code[0]) |
                                         static_cast<u32>(source.conditional_code[1]) << 1;
            }
            if (source.mask & Pica::Shader::DebugDataRecord::COND_BOOL_IN) {
                target.condition_bits |= static_cast<u32>(source.cond_bool) << 2;
            }
            if (source.mask & Pica::Shader::DebugDataRecord::COND_CMP_IN) {
                target.condition_bits |= static_cast<u32>(source.cond_cmp[0]) << 3 |
                                         static_cast<u32>(source.cond_cmp[1]) << 4;
            }
            if (source.mask & Pica::Shader::DebugDataRecord::LOOP_INT_IN) {
                std::memcpy(&target.loop_int, std::addressof(source.loop_int),
                            sizeof(target.loop_int));
            }
            pica_shader_cycles.push_back(target);
        }
        ++pica_shader_generation;
    } else if (operation == PicaShaderOperation::ReadDump) {
        if (generation == pica_shader_generation && start <= pica_shader_dump.size()) {
            const u32 size = std::min(
                {count, MAX_PACKET_DATA_SIZE, static_cast<u32>(pica_shader_dump.size() - start)});
            if (size) {
                std::memcpy(packet.GetPacketData().data(), pica_shader_dump.data() + start, size);
            }
            packet.SetPacketDataSize(size);
        } else {
            packet.SetPacketDataSize(0);
        }
        packet.SendReply();
        return;
    } else if (operation == PicaShaderOperation::ReadCycles) {
        const u32 max_records = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(PicaShaderCycle);
        std::vector<PicaShaderCycle> result;
        if (generation == pica_shader_generation) {
            for (u32 i = std::min(start, static_cast<u32>(pica_shader_cycles.size()));
                 i < pica_shader_cycles.size() && result.size() < std::min(count, max_records);
                 ++i) {
                if (instruction_offset == UINT32_MAX ||
                    pica_shader_cycles[i].instruction_offset == instruction_offset) {
                    result.push_back(pica_shader_cycles[i]);
                }
            }
        }
        const u32 returned = static_cast<u32>(result.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        if (!result.empty()) {
            std::memcpy(packet.GetPacketData().data() + sizeof(returned), result.data(),
                        result.size() * sizeof(result.front()));
        }
        packet.SetPacketDataSize(sizeof(returned) + result.size() * sizeof(result.front()));
        packet.SendReply();
        return;
    } else if (operation == PicaShaderOperation::Clear) {
        pica_shader_dump.clear();
        pica_shader_cycles.clear();
        pica_shader_vertex_input_valid = false;
    }

    const PicaShaderStatus reply{pica_shader_generation, static_cast<u32>(pica_shader_dump.size()),
                                 static_cast<u32>(pica_shader_cycles.size()),
                                 pica_shader_vertex_input_valid};
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

bool RPCServer::ValidatePacket(const PacketHeader& header) const {
    if (header.version == 0 || header.version > CURRENT_VERSION ||
        header.packet_size > MAX_PACKET_DATA_SIZE) {
        return false;
    }
    switch (header.packet_type) {
    case PacketType::ReadMemory:
    case PacketType::ProcessList:
    case PacketType::SetGetProcess:
        return header.packet_size == 2 * sizeof(u32);
    case PacketType::WriteMemory:
        return header.packet_size > 2 * sizeof(u32);
    case PacketType::Capabilities:
        return header.packet_size == 0;
    case PacketType::EmulationControl:
        return header.packet_size >= sizeof(u32);
    case PacketType::PicaSnapshot:
        return header.packet_size >= sizeof(u32) && header.packet_size <= 4 * sizeof(u32);
    case PacketType::PicaBreakpoint:
    case PacketType::PicaTrace:
    case PacketType::PicaShader:
    case PacketType::DebugCapture:
        return header.packet_size >= sizeof(u32) && header.packet_size <= 5 * sizeof(u32);
    case PacketType::PicaTimeline:
        return header.packet_size >= sizeof(u32) && header.packet_size <= 8 * sizeof(u32);
    case PacketType::CPURegisters:
        return header.packet_size == 3 * sizeof(u32) ||
               header.packet_size == 4 * sizeof(u32);
    case PacketType::GXCommandTrace:
    case PacketType::DebugState:
        return header.packet_size >= sizeof(u32) && header.packet_size <= 4 * sizeof(u32);
    case PacketType::PicaRenderTarget:
        return header.packet_size == sizeof(u32);
    default:
        return false;
    }
}

void RPCServer::SendError(Packet& packet, Error error) const {
    const ErrorReply reply{ERROR_REPLY_MAGIC, error};
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandleSingleRequest(std::unique_ptr<Packet> request_packet) {
    bool success = false;
    const auto packet_data = request_packet->GetPacketData();

    if (!ValidatePacket(request_packet->GetHeader())) {
        SendError(*request_packet, Error::InvalidPacket);
        return;
    }
    if (!IsPacketTypeEnabled(request_packet->GetPacketType())) {
        SendError(*request_packet, Error::PermissionDenied);
        return;
    }

    {
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
            if (arg2 > 0 && arg2 <= MAX_PACKET_DATA_SIZE - (sizeof(u32) * 2) &&
                request_packet->GetPacketDataSize() == 2 * sizeof(u32) + arg2) {
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
            const auto operation = static_cast<EmulationControl>(arg1);
            if (arg1 > static_cast<u32>(EmulationControl::FrameAdvance)) {
                break;
            }
            if (!IsEmulationControlEnabled(operation)) {
                SendError(*request_packet, Error::PermissionDenied);
                return;
            }
            if (request_packet->GetPacketDataSize() > sizeof(u32) &&
                operation != EmulationControl::Run && operation != EmulationControl::SaveState &&
                operation != EmulationControl::LoadState &&
                operation != EmulationControl::Screenshot) {
                break;
            }
            const auto path_size = request_packet->GetPacketDataSize() - sizeof(u32);
            const std::string path{reinterpret_cast<const char*>(packet_data.data() + sizeof(u32)),
                                   path_size};
            HandleEmulationControl(*request_packet, operation, path);
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
            u32 arg4 = 0;
            u32 arg5 = UINT32_MAX;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (request_packet->GetPacketDataSize() >= 5 * sizeof(u32)) {
                std::memcpy(&arg5, packet_data.data() + 4 * sizeof(u32), sizeof(arg5));
            }
            HandlePicaBreakpoint(*request_packet, static_cast<PicaBreakpointOperation>(arg1), arg2,
                                 arg3, arg4, arg5);
            success = true;
            break;
        }
        case PacketType::PicaTimeline: {
            u32 arg3 = 20;
            u32 arg4 = UINT32_MAX;
            u32 arg5 = 0;
            u32 arg6 = UINT32_MAX;
            u32 arg7 = UINT32_MAX;
            u32 arg8 = UINT32_MAX;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (request_packet->GetPacketDataSize() >= 5 * sizeof(u32)) {
                std::memcpy(&arg5, packet_data.data() + 4 * sizeof(u32), sizeof(arg5));
            }
            if (request_packet->GetPacketDataSize() >= 6 * sizeof(u32)) {
                std::memcpy(&arg6, packet_data.data() + 5 * sizeof(u32), sizeof(arg6));
            }
            if (request_packet->GetPacketDataSize() >= 7 * sizeof(u32)) {
                std::memcpy(&arg7, packet_data.data() + 6 * sizeof(u32), sizeof(arg7));
            }
            if (request_packet->GetPacketDataSize() >= 8 * sizeof(u32)) {
                std::memcpy(&arg8, packet_data.data() + 7 * sizeof(u32), sizeof(arg8));
            }
            HandlePicaTimeline(*request_packet, static_cast<PicaTimelineOperation>(arg1), arg2,
                               arg3, arg4, arg5, arg6, arg7, arg8);
            success = true;
            break;
        }
        case PacketType::PicaRenderTarget:
            HandlePicaRenderTarget(*request_packet);
            success = true;
            break;
        case PacketType::DebugState: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            HandleDebugState(*request_packet, static_cast<DebugStateOperation>(arg1), arg2, arg3,
                             arg4);
            success = true;
            break;
        }
        case PacketType::DebugCapture: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            u32 arg5 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (request_packet->GetPacketDataSize() >= 5 * sizeof(u32)) {
                std::memcpy(&arg5, packet_data.data() + 4 * sizeof(u32), sizeof(arg5));
            }
            HandleDebugCapture(*request_packet, static_cast<DebugCaptureOperation>(arg1), arg2,
                               arg3, arg4, arg5);
            success = true;
            break;
        }
        case PacketType::PicaTrace: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            u32 arg5 = UINT32_MAX;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (request_packet->GetPacketDataSize() >= 5 * sizeof(u32)) {
                std::memcpy(&arg5, packet_data.data() + 4 * sizeof(u32), sizeof(arg5));
            }
            if (arg4 <= MAX_PACKET_DATA_SIZE) {
                HandlePicaTrace(*request_packet, static_cast<PicaTraceOperation>(arg1), arg2, arg3,
                                arg4, arg5);
                success = true;
            }
            break;
        }
        case PacketType::CPURegisters: {
            u32 arg3 = 1;
            u32 arg4 = 0;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
                HandleCPURegisters(*request_packet, arg1, arg2, arg3, arg4);
            } else {
                HandleCPURegisters(*request_packet, 0, arg1, arg2, arg3);
            }
            success = true;
            break;
        }
        case PacketType::GXCommandTrace: {
            u32 arg3 = 0;
            u32 arg4 = UINT32_MAX;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            HandleGXCommandTrace(*request_packet, static_cast<GXCommandTraceOperation>(arg1), arg2,
                                 arg3, arg4);
            success = true;
            break;
        }
        case PacketType::PicaShader: {
            u32 arg3 = 0;
            u32 arg4 = 0;
            u32 arg5 = UINT32_MAX;
            if (request_packet->GetPacketDataSize() >= 3 * sizeof(u32)) {
                std::memcpy(&arg3, packet_data.data() + 2 * sizeof(u32), sizeof(arg3));
            }
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                std::memcpy(&arg4, packet_data.data() + 3 * sizeof(u32), sizeof(arg4));
            }
            if (request_packet->GetPacketDataSize() >= 5 * sizeof(u32)) {
                std::memcpy(&arg5, packet_data.data() + 4 * sizeof(u32), sizeof(arg5));
            }
            HandlePicaShader(*request_packet, static_cast<PicaShaderOperation>(arg1), arg2, arg3,
                             arg4, arg5);
            success = true;
            break;
        }
        default:
            break;
        }
    }

    if (!success) {
        SendError(*request_packet, Error::InvalidArgument);
    }
}

void RPCServer::HandleRequestsLoop(std::stop_token stop_token) {
    std::unique_ptr<RPC::Packet> request_packet;

    LOG_INFO(RPC_Server, "Request handler started.");

    while ((request_packet = request_queue.PopWait(stop_token))) {
        HandleSingleRequest(std::move(request_packet));
    }
}

void RPCServer::HandleWaitRequestsLoop(std::stop_token stop_token) {
    std::unique_ptr<RPC::Packet> request_packet;
    while ((request_packet = wait_request_queue.PopWait(stop_token))) {
        HandleSingleRequest(std::move(request_packet));
    }
}

void RPCServer::QueueRequest(std::unique_ptr<RPC::Packet> request) {
    if (request && request->GetPacketType() == PacketType::DebugState &&
        request->GetPacketDataSize() >= sizeof(u32)) {
        u32 operation{};
        std::memcpy(&operation, request->GetPacketData().data(), sizeof(operation));
        if (operation == static_cast<u32>(DebugStateOperation::Wait)) {
            wait_request_queue.Push(std::move(request));
            return;
        }
    }
    request_queue.Push(std::move(request));
}

}; // namespace Core::RPC
