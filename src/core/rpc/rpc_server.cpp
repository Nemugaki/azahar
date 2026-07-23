// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/hash.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/rpc/packet.h"
#include "core/rpc/rpc_server.h"
#include "render_debugger/capture_file.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/shader_unit.h"
#include "video_core/pica/vertex_loader.h"
#include "video_core/shader/shader_interpreter.h"

#include <chrono>
#include <thread>

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

RPCServer::RPCServer(Core::System& system_, EmulationControlHandler emulation_control_handler_,
                     std::shared_ptr<Debugger::RenderSessionManager> render_sessions_)
    : system{system_}, emulation_control_handler{std::move(emulation_control_handler_)},
      render_sessions{std::move(render_sessions_)} {
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
    if (pica_trace_owned) {
        Pica::DebugUtils::FinishPicaTracing(Pica::DebugUtils::PicaTraceOwner::RPC);
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
    const std::string build =
        std::string{Common::g_scm_rev} + " " + std::string{Common::g_scm_desc};
    const u32 build_size =
        std::min(static_cast<u32>(build.size()),
                 static_cast<u32>(MAX_PACKET_DATA_SIZE - sizeof(CapabilitiesReply)));
    const CapabilitiesReply response{PROTOCOL_MAGIC, GetEnabledCapabilities(), MAX_PACKET_DATA_SIZE,
                                     build_size};
    std::memcpy(packet.GetPacketData().data(), &response, sizeof(response));
    std::memcpy(packet.GetPacketData().data() + sizeof(response), build.data(), build_size);
    packet.SetPacketDataSize(sizeof(response) + build_size);
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
    if (Settings::values.pica_debugging.GetValue() &&
        (Settings::values.rpc_allow_pica_command_list.GetValue() ||
         Settings::values.rpc_allow_render_captures.GetValue())) {
        capabilities |= CAPABILITY_RENDER_SESSIONS;
    }
    if (Settings::values.pica_debugging.GetValue() &&
        Settings::values.rpc_allow_render_captures.GetValue()) {
        capabilities |= CAPABILITY_RENDER_OUTPUTS | CAPABILITY_RENDER_DEBUG;
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
    case PacketType::RenderSession:
        return capabilities & CAPABILITY_RENDER_SESSIONS;
    case PacketType::RenderOutput:
        return capabilities & CAPABILITY_RENDER_OUTPUTS;
    case PacketType::RenderDebug:
        return capabilities & CAPABILITY_RENDER_DEBUG;
    default:
        return false;
    }
}

void RPCServer::HandleRenderSession(Packet& packet, RenderSessionOperation operation, u64 id,
                                    u32 start, u32 count, const std::string& path) {
    const bool file_operation =
        operation == RenderSessionOperation::Import || operation == RenderSessionOperation::Export;
    if (file_operation && !Settings::values.rpc_allow_render_captures.GetValue()) {
        SendError(packet, Error::PermissionDenied);
        return;
    }
    if (!render_sessions) {
        SendError(packet, Error::InvalidState);
        return;
    }
    const auto& sessions = render_sessions;
    if (path.find('\0') != std::string::npos) {
        SendError(packet, Error::InvalidArgument);
        return;
    }
    const auto make_reply = [&](const Debugger::SessionDescriptor& descriptor) {
        RenderSessionReply reply{descriptor.id, descriptor.capabilities};
        reply.flags = static_cast<u32>(descriptor.live) |
                      (static_cast<u32>(sessions->GetActiveId() == descriptor.id) << 1) |
                      (static_cast<u32>(descriptor.complete) << 2) |
                      (static_cast<u32>(descriptor.truncated) << 3);
        std::memcpy(reply.producer.data(), descriptor.producer.data(),
                    std::min(reply.producer.size(), descriptor.producer.size()));
        std::memcpy(reply.backend.data(), descriptor.backend.data(),
                    std::min(reply.backend.size(), descriptor.backend.size()));
        return reply;
    };
    if (operation == RenderSessionOperation::List) {
        const auto descriptors = sessions->List();
        const u32 max_count = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(RenderSessionReply);
        const u32 returned =
            start >= descriptors.size()
                ? 0
                : std::min({count, max_count, static_cast<u32>(descriptors.size() - start)});
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 index = 0; index < returned; ++index) {
            const auto reply = make_reply(descriptors[start + index]);
            std::memcpy(packet.GetPacketData().data() + sizeof(returned) + index * sizeof(reply),
                        &reply, sizeof(reply));
        }
        packet.SetPacketDataSize(sizeof(returned) + returned * sizeof(RenderSessionReply));
        packet.SendReply();
        return;
    }

    if (operation == RenderSessionOperation::Select) {
        if (!sessions->Select(id)) {
            SendError(packet, Error::NotFound);
            return;
        }
    } else if (operation == RenderSessionOperation::Remove) {
        if (!sessions->Remove(id)) {
            SendError(packet, Error::NotFound);
            return;
        }
        id = sessions->GetActiveId();
    } else if (operation == RenderSessionOperation::Import) {
        Debugger::Capture capture;
        Debugger::CaptureReadLimits limits;
        limits.total_bytes = static_cast<u64>(Settings::values.debugger_cache_mb.GetValue()) << 20;
        limits.capture.owned_bytes = limits.total_bytes;
        std::string error;
        if (path.empty() || !Debugger::LoadCapture(path, capture, error, limits)) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        id = sessions->AddImported(std::move(capture));
        if (!id) {
            SendError(packet, Error::Failed);
            return;
        }
    } else if (operation == RenderSessionOperation::Export) {
        if (!id) {
            id = sessions->GetActiveId();
        }
        Debugger::Capture capture;
        std::string error;
        if (path.empty() || !sessions->Snapshot(id, capture)) {
            SendError(packet, path.empty() ? Error::InvalidArgument : Error::NotFound);
            return;
        }
        if (!Debugger::SaveCapture(path, capture, error)) {
            SendError(packet, Error::Failed);
            return;
        }
    }

    const auto descriptors = sessions->List();
    const auto descriptor =
        std::ranges::find_if(descriptors, [id](const auto& value) { return value.id == id; });
    if (descriptor == descriptors.end()) {
        SendError(packet, Error::NotFound);
        return;
    }
    const auto reply = make_reply(*descriptor);
    std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
    packet.SetPacketDataSize(sizeof(reply));
    packet.SendReply();
}

void RPCServer::HandleRenderOutput(Packet& packet, RenderOutputOperation operation, u64 session_id,
                                   u32 sequence, u32 offset, u32 count) {
    if (!render_sessions) {
        SendError(packet, Error::InvalidState);
        return;
    }
    const auto& sessions = render_sessions;
    if (!session_id) {
        session_id = sessions->GetActiveId();
    }
    const auto session = sessions->Get(session_id);
    if (!session) {
        SendError(packet, Error::NotFound);
        return;
    }
    if (operation == RenderOutputOperation::Status) {
        const auto output = session->GetDrawOutput(sequence);
        if (!output) {
            SendError(packet, Error::NotFound);
            return;
        }
        const RenderOutputReply reply{
            session_id,    sequence,       output->format, output->address,
            output->width, output->height, output->stride, static_cast<u32>(output->bytes.size())};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }
    const auto output = session->GetDrawOutput(sequence, Debugger::CapturePhase::PostDraw, offset,
                                               std::min(count, MAX_PACKET_DATA_SIZE));
    if (!output) {
        SendError(packet, Error::InvalidArgument);
        return;
    }
    std::memcpy(packet.GetPacketData().data(), output->bytes.data(), output->bytes.size());
    packet.SetPacketDataSize(static_cast<u32>(output->bytes.size()));
    packet.SendReply();
}

void RPCServer::HandleRenderDebug(Packet& packet, RenderDebugOperation operation,
                                  std::span<const u8> request) {
    const auto read_u32 = [&request](std::size_t offset, u32& value) {
        if (offset + sizeof(value) > request.size()) {
            return false;
        }
        std::memcpy(&value, request.data() + offset, sizeof(value));
        return true;
    };
    const auto read_u64 = [&read_u32](std::size_t offset, u64& value) {
        u32 low{}, high{};
        if (!read_u32(offset, low) || !read_u32(offset + sizeof(u32), high)) {
            return false;
        }
        value = low | static_cast<u64>(high) << 32;
        return true;
    };
    const auto context = Pica::g_debug_context;
    if (!context) {
        SendError(packet, Error::InvalidState);
        return;
    }
    const auto& sessions = render_sessions;
    if (!sessions) {
        SendError(packet, Error::InvalidState);
        return;
    }

    if (operation == RenderDebugOperation::Capabilities) {
        const std::string build =
            std::string{Common::g_scm_rev} + " " + std::string{Common::g_scm_desc};
        const u32 build_size = std::min<u32>(
            build.size(), MAX_PACKET_DATA_SIZE - sizeof(RenderDebugCapabilitiesReply));
        const RenderDebugCapabilitiesReply reply{Debugger::Timeline | Debugger::RegisterWrites |
                                                     Debugger::Resources | Debugger::Shaders |
                                                     Debugger::RegisterState | Debugger::Outputs,
                                                 MAX_PACKET_DATA_SIZE, build_size};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        std::memcpy(packet.GetPacketData().data() + sizeof(reply), build.data(), build_size);
        packet.SetPacketDataSize(sizeof(reply) + build_size);
        packet.SendReply();
        return;
    }

    if (operation == RenderDebugOperation::DrawBreak) {
        u32 command{}, frame_draw{}, color_address{}, filter_color{};
        if (!read_u32(sizeof(u32), command) || command > 2) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        if (command == 0) {
            if (!read_u32(2 * sizeof(u32), frame_draw) ||
                !read_u32(3 * sizeof(u32), color_address) ||
                !read_u32(4 * sizeof(u32), filter_color) || filter_color > 1 ||
                frame_draw == std::numeric_limits<u32>::max()) {
                SendError(packet, Error::InvalidArgument);
                return;
            }
            context->ArmDrawBreak(frame_draw,
                                  filter_color ? std::optional<u32>{color_address} : std::nullopt);
        } else if (command == 2) {
            context->CancelDrawBreak();
        }
        const auto state = context->GetDrawBreakState();
        const RenderDrawBreakReply reply{state.phase, state.frame_draw, state.color_address,
                                         state.filter_color};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }

    if (operation == RenderDebugOperation::CaptureFrame) {
        u32 timeout_ms{};
        if (!read_u32(sizeof(u32), timeout_ms) || request.size() <= 2 * sizeof(u32) ||
            !emulation_control_handler) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        const std::string path{reinterpret_cast<const char*>(request.data() + 2 * sizeof(u32)),
                               request.size() - 2 * sizeof(u32)};
        if (path.find('\0') != std::string::npos || path.empty()) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        std::unique_lock capture_lock{render_capture_mutex, std::try_to_lock};
        if (!capture_lock.owns_lock()) {
            SendError(packet, Error::Busy);
            return;
        }
        const auto before = system.GetDebugState();
        if (before.reason == Core::DebugPauseReason::Stopped ||
            before.reason == Core::DebugPauseReason::Running) {
            SendError(packet, Error::InvalidState);
            return;
        }
        const auto live = sessions->GetLive();
        live->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::RPC, true);
        const auto release_capture = [&] {
            live->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::RPC, false);
        };
        const auto advance = [&] {
            const auto result =
                emulation_control_handler(EmulationControl::FrameAdvance, std::string{});
            if (result.result != EmulationResult::Success) {
                return false;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(std::clamp(timeout_ms, 1U, 60000U));
            while (!system.frame_limiter.IsWaitingForFrameAdvance() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return system.frame_limiter.IsWaitingForFrameAdvance();
        };
        if ((before.reason == Core::DebugPauseReason::CPU ||
             before.reason == Core::DebugPauseReason::Pica) &&
            !advance()) {
            release_capture();
            SendError(packet, Error::Failed);
            return;
        }
        live->Clear();
        if (!advance()) {
            release_capture();
            SendError(packet, Error::Failed);
            return;
        }
        release_capture();
        Debugger::Capture capture;
        live->Snapshot(capture);
        capture.producer = std::string{"Azahar "} + Common::g_scm_rev + " " + Common::g_scm_desc;
        capture.backend = "Pica";
        const auto issues = live->Validate(true);
        capture.complete &= issues.empty();
        capture.truncated |= !issues.empty();
        for (const auto& issue : issues) {
            if (issue.kind != Debugger::CaptureIssueKind::Gap &&
                issue.kind != Debugger::CaptureIssueKind::Truncated) {
                capture.gaps.push_back(
                    {issue.frame, issue.draw, issue.role, issue.slot, issue.message});
            }
        }
        std::string error;
        if (!Debugger::SaveCapture(path, capture, error)) {
            SendError(packet, Error::Failed);
            return;
        }
        const u64 id = sessions->AddImported(std::move(capture));
        if (!id) {
            SendError(packet, Error::Failed);
            return;
        }
        const auto session = sessions->Get(id);
        const auto status = session ? session->GetStatus() : Debugger::TimelineStatus{};
        const RenderCaptureReply reply{id, status.count, static_cast<u32>(issues.size()),
                                       issues.empty(), status.truncated};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }

    u64 session_id{};
    if (!read_u64(sizeof(u32), session_id)) {
        SendError(packet, Error::InvalidArgument);
        return;
    }
    if (!session_id) {
        session_id = sessions->GetActiveId();
    }
    const auto session = sessions->Get(session_id);
    if (!session) {
        SendError(packet, Error::NotFound);
        return;
    }

    if (operation == RenderDebugOperation::Diff) {
        u64 other_id{};
        u32 left_sequence{}, right_sequence{};
        if (!read_u64(3 * sizeof(u32), other_id) || !read_u32(5 * sizeof(u32), left_sequence) ||
            !read_u32(6 * sizeof(u32), right_sequence)) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        if (!other_id) {
            other_id = sessions->GetActiveId();
        }
        const auto other = sessions->Get(other_id);
        const auto left_state = session->GetDrawState(left_sequence);
        const auto right_state = other ? other->GetDrawState(right_sequence) : std::nullopt;
        if (!left_state || !right_state) {
            SendError(packet, Error::NotFound);
            return;
        }
        RenderDiffReply reply{};
        for (std::size_t index = 0; index < left_state->size(); ++index) {
            reply.changed_registers += (*left_state)[index] != (*right_state)[index];
        }
        reply.changed_mask |= reply.changed_registers ? 1U : 0U;

        Debugger::Capture left_capture;
        Debugger::Capture right_capture;
        sessions->Snapshot(session_id, left_capture);
        sessions->Snapshot(other_id, right_capture);
        const auto find_draw = [](const Debugger::Capture& capture, u32 sequence) {
            return std::ranges::find_if(capture.draws, [sequence](const auto& draw) {
                return draw.timeline.sequence == sequence;
            });
        };
        const auto left_draw = find_draw(left_capture, left_sequence);
        const auto right_draw = find_draw(right_capture, right_sequence);
        if (left_draw != left_capture.draws.end() && right_draw != right_capture.draws.end()) {
            reply.changed_mask |=
                left_draw->timeline.target.color_address !=
                            right_draw->timeline.target.color_address ||
                        left_draw->timeline.target.depth_address !=
                            right_draw->timeline.target.depth_address ||
                        left_draw->timeline.target.width != right_draw->timeline.target.width ||
                        left_draw->timeline.target.height != right_draw->timeline.target.height ||
                        left_draw->timeline.target.color_format !=
                            right_draw->timeline.target.color_format
                    ? 2U
                    : 0U;
            reply.changed_mask |= left_draw->shader_id != right_draw->shader_id ? 4U : 0U;
            const auto same_reference = [&](const auto& reference, const auto& draw,
                                            const auto& capture) {
                const auto match = std::ranges::find_if(draw.resources, [&](const auto& value) {
                    return value.role == reference.role && value.slot == reference.slot &&
                           value.phase == reference.phase;
                });
                if (match == draw.resources.end()) {
                    return false;
                }
                const auto left_resource = std::ranges::find_if(
                    left_capture.resources,
                    [id = reference.resource_id](const auto& value) { return value.id == id; });
                const auto right_resource = std::ranges::find_if(
                    capture.resources,
                    [id = match->resource_id](const auto& value) { return value.id == id; });
                return left_resource != left_capture.resources.end() &&
                       right_resource != capture.resources.end() &&
                       left_resource->role == right_resource->role &&
                       left_resource->format == right_resource->format &&
                       left_resource->width == right_resource->width &&
                       left_resource->height == right_resource->height &&
                       left_resource->stride == right_resource->stride &&
                       left_resource->bytes == right_resource->bytes;
            };
            for (const auto& reference : left_draw->resources) {
                reply.changed_resources += !same_reference(reference, *right_draw, right_capture);
            }
            for (const auto& reference : right_draw->resources) {
                const bool present =
                    std::ranges::any_of(left_draw->resources, [&](const auto& value) {
                        return value.role == reference.role && value.slot == reference.slot &&
                               value.phase == reference.phase;
                    });
                reply.changed_resources += !present;
            }
            reply.changed_mask |= reply.changed_resources ? 8U : 0U;
        }

        const auto left_output =
            session->GetDrawOutput(left_sequence, Debugger::CapturePhase::PostDraw);
        const auto right_output =
            other->GetDrawOutput(right_sequence, Debugger::CapturePhase::PostDraw);
        if (left_output && right_output && left_output->width == right_output->width &&
            left_output->height == right_output->height &&
            left_output->stride == right_output->stride) {
            reply.min_x = left_output->width;
            reply.min_y = left_output->height;
            const u32 bytes_per_pixel =
                left_output->width ? left_output->stride / left_output->width : 0;
            const std::size_t size =
                std::min(left_output->bytes.size(), right_output->bytes.size());
            for (std::size_t offset = 0; bytes_per_pixel && offset < size;
                 offset += bytes_per_pixel) {
                const bool changed = !std::equal(left_output->bytes.begin() + offset,
                                                 left_output->bytes.begin() +
                                                     std::min(size, offset + bytes_per_pixel),
                                                 right_output->bytes.begin() + offset);
                if (!changed) {
                    continue;
                }
                ++reply.changed_pixels;
                const u32 pixel = static_cast<u32>(offset / bytes_per_pixel);
                const u32 x = pixel % left_output->width;
                const u32 y = pixel / left_output->width;
                reply.min_x = std::min(reply.min_x, x);
                reply.min_y = std::min(reply.min_y, y);
                reply.max_x = std::max(reply.max_x, x);
                reply.max_y = std::max(reply.max_y, y);
            }
            if (!reply.changed_pixels) {
                reply.min_x = reply.min_y = reply.max_x = reply.max_y = 0;
            }
            reply.changed_mask |= reply.changed_pixels ? 16U : 0U;
        } else if (left_output || right_output) {
            reply.changed_mask |= 16U;
            reply.changed_pixels = std::numeric_limits<u32>::max();
        }
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }

    if (operation == RenderDebugOperation::Vertex) {
        u32 sequence{}, ordinal{};
        if (!read_u32(3 * sizeof(u32), sequence) || !read_u32(4 * sizeof(u32), ordinal)) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        Debugger::Capture capture;
        if (!sessions->Snapshot(session_id, capture)) {
            SendError(packet, Error::NotFound);
            return;
        }
        const auto draw = std::ranges::find_if(capture.draws, [sequence](const auto& value) {
            return value.timeline.sequence == sequence;
        });
        if (draw == capture.draws.end() || ordinal >= draw->timeline.draw_info.vertex_count) {
            SendError(packet,
                      draw == capture.draws.end() ? Error::NotFound : Error::InvalidArgument);
            return;
        }
        const auto shader = std::ranges::find_if(
            capture.shaders, [id = draw->shader_id](const auto& value) { return value.id == id; });
        if (shader == capture.shaders.end()) {
            SendError(packet, Error::NotFound);
            return;
        }
        Pica::RegsInternal regs{};
        std::ranges::copy(draw->registers, regs.reg_array.begin());
        const auto& pipeline = regs.pipeline;
        const PAddr base = pipeline.vertex_attributes.GetPhysicalBaseAddress();
        u32 resolved = ordinal + pipeline.vertex_offset;
        if (draw->timeline.draw_info.mode == Debugger::DrawMode::Indexed) {
            const auto reference = std::ranges::find_if(draw->resources, [](const auto& value) {
                return value.role == Debugger::ResourceRole::IndexBuffer;
            });
            const auto resource =
                reference == draw->resources.end()
                    ? capture.resources.end()
                    : std::ranges::find_if(capture.resources,
                                           [id = reference->resource_id](const auto& value) {
                                               return value.id == id;
                                           });
            const u32 width = pipeline.index_array.format != 0 ? sizeof(u16) : sizeof(u8);
            if (resource == capture.resources.end() ||
                static_cast<u64>(ordinal + 1) * width > resource->bytes.size()) {
                SendError(packet, Error::NotFound);
                return;
            }
            if (width == sizeof(u16)) {
                u16 value{};
                std::memcpy(&value, resource->bytes.data() + ordinal * width, sizeof(value));
                resolved = value;
            } else {
                resolved = resource->bytes[ordinal];
            }
        }
        const auto read_captured = [&capture](PAddr address, std::size_t size) -> const u8* {
            for (const auto& resource : capture.resources) {
                if (resource.role != Debugger::ResourceRole::VertexBuffer ||
                    address < resource.address) {
                    continue;
                }
                const u64 offset = static_cast<u64>(address) - resource.address;
                if (offset <= resource.bytes.size() &&
                    size <= resource.bytes.size() - static_cast<std::size_t>(offset)) {
                    return resource.bytes.data() + offset;
                }
            }
            return nullptr;
        };
        Pica::AttributeBuffer defaults{};
        Pica::AttributeBuffer input{};
        using FloatUniforms = std::array<std::array<float, 4>, 96>;
        using IntegerUniforms = std::array<std::array<u8, 4>, 4>;
        using BoolUniforms = std::array<bool, 16>;
        using FixedAttributes = std::array<std::array<float, 4>, 16>;
        using FixedWritten = std::array<bool, 16>;
        using InputMap = std::array<u8, 16>;
        constexpr std::size_t ExpectedStateSize = sizeof(FloatUniforms) + sizeof(IntegerUniforms) +
                                                  sizeof(BoolUniforms) + sizeof(FixedAttributes) +
                                                  sizeof(FixedWritten) + sizeof(InputMap) +
                                                  sizeof(std::array<std::array<u8, 4>, 7>);
        if (shader->state.size() != ExpectedStateSize ||
            shader->code.size() > Pica::MAX_PROGRAM_CODE_LENGTH ||
            shader->metadata.size() > Pica::MAX_SWIZZLE_DATA_LENGTH) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        std::size_t state_offset{};
        const auto take_state = [&](auto& value) {
            std::memcpy(&value, shader->state.data() + state_offset, sizeof(value));
            state_offset += sizeof(value);
        };
        FloatUniforms float_uniforms{};
        IntegerUniforms integer_uniforms{};
        BoolUniforms bool_uniforms{};
        FixedAttributes fixed_attributes{};
        FixedWritten fixed_written{};
        InputMap input_map{};
        std::array<std::array<u8, 4>, 7> output_map{};
        take_state(float_uniforms);
        take_state(integer_uniforms);
        take_state(bool_uniforms);
        take_state(fixed_attributes);
        take_state(fixed_written);
        take_state(input_map);
        take_state(output_map);
        for (std::size_t attribute = 0; attribute < fixed_attributes.size(); ++attribute) {
            for (std::size_t component = 0; component < 4; ++component) {
                defaults[attribute][component] =
                    Pica::f24::FromFloat32(fixed_attributes[attribute][component]);
            }
        }
        const Pica::VertexLoader loader{read_captured, pipeline};
        if (!loader.LoadVertex(base, ordinal, resolved, input, defaults)) {
            SendError(packet, Error::NotFound);
            return;
        }

        Pica::ShaderSetup setup;
        for (std::size_t reg = 0; reg < float_uniforms.size(); ++reg) {
            for (std::size_t component = 0; component < 4; ++component) {
                setup.uniforms.f[reg][component] =
                    Pica::f24::FromFloat32(float_uniforms[reg][component]);
            }
        }
        setup.uniforms.b = bool_uniforms;
        for (std::size_t reg = 0; reg < integer_uniforms.size(); ++reg) {
            for (std::size_t component = 0; component < 4; ++component) {
                setup.uniforms.i[reg][component] = integer_uniforms[reg][component];
            }
        }
        for (std::size_t index = 0; index < shader->code.size(); ++index) {
            setup.UpdateProgramCode(index, shader->code[index]);
        }
        for (std::size_t index = 0; index < shader->metadata.size(); ++index) {
            setup.UpdateSwizzleData(index, shader->metadata[index]);
        }
        Pica::Shader::InterpreterEngine engine;
        engine.SetupBatch(setup, shader->entry_point);
        Pica::ShaderUnit unit;
        Pica::AttributeBuffer output{};
        unit.LoadInput(regs.vs, input);
        engine.Run(setup, unit);
        unit.WriteOutput(regs.vs, output);
        regs.rasterizer.ValidateSemantics();
        const Pica::OutputVertex semantic{regs.rasterizer, output};

        RenderVertexReply reply{};
        reply.ordinal = ordinal;
        reply.resolved_index = resolved;
        reply.shader_entry = shader->entry_point;
        reply.shader_id = Common::HashCombine(
            Common::ComputeHash64(shader->code.data(), shader->code.size() * sizeof(u32)),
            Common::ComputeHash64(shader->metadata.data(), shader->metadata.size() * sizeof(u32)),
            Common::ComputeHash64(shader->state.data(), shader->state.size()));
        reply.input_map = input_map;
        for (std::size_t attribute = 0; attribute < 16; ++attribute) {
            const bool fixed = pipeline.vertex_attributes.IsDefaultAttribute(attribute);
            reply.fixed_mask |= static_cast<u32>(fixed) << attribute;
            reply.loaded_mask |=
                static_cast<u32>(!fixed &&
                                 pipeline.vertex_attributes.GetNumElements(attribute) != 0)
                << attribute;
            for (std::size_t component = 0; component < 4; ++component) {
                reply.input[attribute * 4 + component] = input[attribute][component].ToFloat32();
                reply.output[attribute * 4 + component] = output[attribute][component].ToFloat32();
            }
        }
        std::size_t semantic_index{};
        const auto append_semantic = [&](const auto& value) {
            for (std::size_t component = 0; component < sizeof(value) / sizeof(*value.AsArray());
                 ++component) {
                reply.semantic[semantic_index++] = value.AsArray()[component].ToFloat32();
            }
        };
        append_semantic(semantic.pos);
        append_semantic(semantic.quat);
        append_semantic(semantic.color);
        append_semantic(semantic.tc0);
        append_semantic(semantic.tc1);
        reply.semantic[semantic_index++] = semantic.tc0_w.ToFloat32();
        append_semantic(semantic.view);
        append_semantic(semantic.tc2);
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    }

    if (operation == RenderDebugOperation::Timeline) {
        u64 epoch{};
        u32 start{}, count{}, frame{};
        if (!read_u64(3 * sizeof(u32), epoch) || !read_u32(5 * sizeof(u32), start) ||
            !read_u32(6 * sizeof(u32), count) || !read_u32(7 * sizeof(u32), frame)) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        const u32 maximum =
            (MAX_PACKET_DATA_SIZE - sizeof(RenderTimelinePageReply)) / sizeof(PicaTimelineEntry);
        const auto page = session->QueryPage(
            epoch, {.start = start, .count = std::min(count, maximum), .frame = frame});
        const RenderTimelinePageReply header{
            page.epoch, page.next_sequence, static_cast<u32>(page.entries.size()),
            static_cast<u32>(page.has_more) | static_cast<u32>(page.stale) << 1};
        std::memcpy(packet.GetPacketData().data(), &header, sizeof(header));
        for (std::size_t index = 0; index < page.entries.size(); ++index) {
            const auto& source = page.entries[index];
            const PicaTimelineEntry target{
                source.sequence,
                static_cast<u32>(source.kind),
                source.frame,
                source.draw,
                source.frame_draw,
                source.changed_mask,
                {source.target.color_address, source.target.depth_address, source.target.width,
                 source.target.height, source.target.color_format, source.target.depth_format},
                static_cast<u32>(source.draw_info.mode),
                source.draw_info.vertex_count,
                source.draw_info.topology,
                source.draw_info.vertex_offset,
                source.draw_info.vertex_shader_entry};
            std::memcpy(packet.GetPacketData().data() + sizeof(header) + index * sizeof(target),
                        &target, sizeof(target));
        }
        packet.SetPacketDataSize(sizeof(header) + page.entries.size() * sizeof(PicaTimelineEntry));
        packet.SendReply();
        return;
    }

    u32 sequence{}, start{}, count{};
    if (!read_u32(3 * sizeof(u32), sequence) || !read_u32(4 * sizeof(u32), start) ||
        !read_u32(5 * sizeof(u32), count)) {
        SendError(packet, Error::InvalidArgument);
        return;
    }
    if (operation == RenderDebugOperation::State) {
        const auto state = session->GetDrawState(sequence);
        if (!state || start > state->size()) {
            SendError(packet, state ? Error::InvalidArgument : Error::NotFound);
            return;
        }
        const u32 returned =
            std::min<u32>({count, static_cast<u32>(state->size() - start),
                           (MAX_PACKET_DATA_SIZE - sizeof(RenderStatePageReply)) / sizeof(u32)});
        const RenderStatePageReply reply{static_cast<u32>(state->size()), start, returned};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        std::memcpy(packet.GetPacketData().data() + sizeof(reply), state->data() + start,
                    returned * sizeof(u32));
        packet.SetPacketDataSize(sizeof(reply) + returned * sizeof(u32));
        packet.SendReply();
        return;
    }
    if (operation == RenderDebugOperation::Validate) {
        const auto issues = session->Validate(sequence != 0);
        const u32 maximum =
            (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(RenderValidationIssueReply);
        const u32 returned =
            start >= issues.size()
                ? 0
                : std::min({count, maximum, static_cast<u32>(issues.size() - start)});
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 index = 0; index < returned; ++index) {
            const auto& issue = issues[start + index];
            RenderValidationIssueReply reply{static_cast<u32>(issue.kind), issue.frame, issue.draw,
                                             static_cast<u32>(issue.role), issue.slot};
            std::memcpy(reply.message.data(), issue.message.data(),
                        std::min(reply.message.size(), issue.message.size()));
            std::memcpy(packet.GetPacketData().data() + sizeof(returned) + index * sizeof(reply),
                        &reply, sizeof(reply));
        }
        packet.SetPacketDataSize(sizeof(returned) + returned * sizeof(RenderValidationIssueReply));
        packet.SendReply();
        return;
    }
    if (operation == RenderDebugOperation::OutputStatus ||
        operation == RenderDebugOperation::OutputRead) {
        const auto phase = static_cast<Debugger::CapturePhase>(start);
        if (phase > Debugger::CapturePhase::PostDraw) {
            SendError(packet, Error::InvalidArgument);
            return;
        }
        const auto output = session->GetDrawOutput(
            sequence, phase, operation == RenderDebugOperation::OutputRead ? count : 0,
            operation == RenderDebugOperation::OutputRead ? MAX_PACKET_DATA_SIZE
                                                          : std::numeric_limits<u64>::max());
        if (!output) {
            SendError(packet, Error::NotFound);
            return;
        }
        if (operation == RenderDebugOperation::OutputRead) {
            std::memcpy(packet.GetPacketData().data(), output->bytes.data(), output->bytes.size());
            packet.SetPacketDataSize(static_cast<u32>(output->bytes.size()));
        } else {
            const RenderOutputInfoReply reply{session_id,
                                              sequence,
                                              static_cast<u32>(phase),
                                              output->address,
                                              output->format,
                                              output->width,
                                              output->height,
                                              output->stride,
                                              static_cast<u32>(output->tiling),
                                              static_cast<u32>(output->origin),
                                              static_cast<u32>(output->bytes.size()),
                                              0};
            std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
            packet.SetPacketDataSize(sizeof(reply));
        }
        packet.SendReply();
        return;
    }
    SendError(packet, Error::Unsupported);
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
        const PicaBreakpointConditionReply reply{static_cast<u32>(condition.field), condition.value,
                                                 condition.mask};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
    } else if ((operation == PicaBreakpointOperation::SetOptions ||
                operation == PicaBreakpointOperation::GetOptions) &&
               event < static_cast<u32>(Pica::DebugContext::Event::NumEvents)) {
        if (operation == PicaBreakpointOperation::SetOptions) {
            context->SetBreakpointOptions(static_cast<Pica::DebugContext::Event>(event),
                                          argument != 0, value);
        }
        const auto options =
            context->GetBreakpointOptions(static_cast<Pica::DebugContext::Event>(event));
        const PicaBreakpointOptionsReply reply{options.one_shot, options.skip_remaining,
                                               options.hit_count};
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
    const auto& sessions = render_sessions;
    if (!sessions) {
        SendError(packet, Error::InvalidState);
        return;
    }
    const auto session = sessions->GetActive();
    if (operation == PicaTimelineOperation::Clear) {
        if (sessions->GetActiveId() != Debugger::RenderSessionManager::LiveSessionId) {
            SendError(packet, Error::InvalidState);
            return;
        }
        session->Clear();
    } else if (operation == PicaTimelineOperation::Read) {
        const u32 max_entries = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(PicaTimelineEntry);
        const bool filter_kind = kind <= static_cast<u32>(Debugger::TimelineKind::Frame);
        const auto entries =
            session->Query({start, std::min(count, max_entries),
                            static_cast<Debugger::TimelineKind>(filter_kind ? kind : 0),
                            filter_kind, required_changes, target_address, shader_entry, frame});
        const u32 returned = static_cast<u32>(entries.size());
        std::memcpy(packet.GetPacketData().data(), &returned, sizeof(returned));
        for (u32 i = 0; i < returned; ++i) {
            const auto& source = entries[i];
            const PicaTimelineEntry target{
                source.sequence,
                static_cast<u32>(source.kind),
                source.frame,
                source.draw,
                source.frame_draw,
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
    const auto status = session->GetStatus();
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
        if (!render_sessions) {
            packet.SetPacketDataSize(0);
            packet.SendReply();
            return;
        }
        const auto target = render_sessions->GetActive()->GetRenderTarget();
        const PicaRenderTargetReply reply{target.color_address, target.depth_address,
                                          target.width,         target.height,
                                          target.color_format,  target.depth_format};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
    }
    packet.SendReply();
}

void RPCServer::HandlePicaTrace(Packet& packet, PicaTraceOperation operation, u32 generation,
                                u32 start, u32 count, u32 register_id) {
    if (operation == PicaTraceOperation::Start) {
        if (!pica_trace_owned) {
            pica_trace_owned =
                Pica::DebugUtils::StartPicaTracing(Pica::DebugUtils::PicaTraceOwner::RPC);
        }
    } else if (operation == PicaTraceOperation::Stop) {
        if (pica_trace_owned) {
            if (auto trace =
                    Pica::DebugUtils::FinishPicaTracing(Pica::DebugUtils::PicaTraceOwner::RPC)) {
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
        const auto changes =
            system.DiffDebugCaptures(id, argument, start, std::min(count, max_entries));
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
        const u32 max_entries = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(DebugCaptureReply);
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
        const DebugCaptureReply reply{id, 0, 0, 0, 0};
        std::memcpy(packet.GetPacketData().data(), &reply, sizeof(reply));
        packet.SetPacketDataSize(sizeof(reply));
        packet.SendReply();
        return;
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

    const auto info = operation == DebugCaptureOperation::Create ? system.CreateDebugCapture()
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
    if (header.magic != PROTOCOL_MAGIC || header.packet_size > MAX_PACKET_DATA_SIZE) {
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
        return header.packet_size == 3 * sizeof(u32) || header.packet_size == 4 * sizeof(u32);
    case PacketType::GXCommandTrace:
    case PacketType::DebugState:
        return header.packet_size >= sizeof(u32) && header.packet_size <= 4 * sizeof(u32);
    case PacketType::PicaRenderTarget:
        return header.packet_size == sizeof(u32);
    case PacketType::RenderSession:
        return header.packet_size >= 5 * sizeof(u32) && header.packet_size <= MAX_PACKET_DATA_SIZE;
    case PacketType::RenderOutput:
        return header.packet_size >= 4 * sizeof(u32) && header.packet_size <= 6 * sizeof(u32);
    case PacketType::RenderDebug:
        return header.packet_size >= sizeof(u32) && header.packet_size <= MAX_PACKET_DATA_SIZE;
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
    const auto read_arg = [&](std::size_t index, u32 fallback = 0) {
        const std::size_t offset = index * sizeof(u32);
        if (request_packet->GetPacketDataSize() < offset + sizeof(u32)) {
            return fallback;
        }
        u32 value;
        std::memcpy(&value, packet_data.data() + offset, sizeof(value));
        return value;
    };

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
        const u32 arg1 = read_arg(0);
        const u32 arg2 = read_arg(1);

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
            if (arg1 > 1) {
                break;
            }
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
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            if (arg1 <= static_cast<u32>(PicaSnapshotOperation::Clear) &&
                arg4 <= MAX_PACKET_DATA_SIZE) {
                HandlePicaSnapshot(*request_packet, static_cast<PicaSnapshotOperation>(arg1), arg2,
                                   arg3, arg4);
                success = true;
            }
            break;
        }
        case PacketType::PicaBreakpoint: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            const u32 arg5 = read_arg(4, UINT32_MAX);
            if (arg1 <= static_cast<u32>(PicaBreakpointOperation::GetOptions)) {
                HandlePicaBreakpoint(*request_packet, static_cast<PicaBreakpointOperation>(arg1),
                                     arg2, arg3, arg4, arg5);
                success = true;
            }
            break;
        }
        case PacketType::PicaTimeline: {
            const u32 arg3 = read_arg(2, 20);
            const u32 arg4 = read_arg(3, UINT32_MAX);
            const u32 arg5 = read_arg(4);
            const u32 arg6 = read_arg(5, UINT32_MAX);
            const u32 arg7 = read_arg(6, UINT32_MAX);
            const u32 arg8 = read_arg(7, UINT32_MAX);
            if (arg1 <= static_cast<u32>(PicaTimelineOperation::Status)) {
                HandlePicaTimeline(*request_packet, static_cast<PicaTimelineOperation>(arg1), arg2,
                                   arg3, arg4, arg5, arg6, arg7, arg8);
                success = true;
            }
            break;
        }
        case PacketType::PicaRenderTarget:
            HandlePicaRenderTarget(*request_packet);
            success = true;
            break;
        case PacketType::DebugState: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            if (arg1 <= static_cast<u32>(DebugStateOperation::Wait)) {
                HandleDebugState(*request_packet, static_cast<DebugStateOperation>(arg1), arg2,
                                 arg3, arg4);
                success = true;
            }
            break;
        }
        case PacketType::DebugCapture: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            const u32 arg5 = read_arg(4);
            if (arg1 <= static_cast<u32>(DebugCaptureOperation::CacheStatus)) {
                HandleDebugCapture(*request_packet, static_cast<DebugCaptureOperation>(arg1), arg2,
                                   arg3, arg4, arg5);
                success = true;
            }
            break;
        }
        case PacketType::RenderSession: {
            const u32 id_low = read_arg(1);
            const u32 id_high = read_arg(2);
            const u32 start = read_arg(3);
            const u32 count = read_arg(4);
            const auto operation = static_cast<RenderSessionOperation>(arg1);
            if (arg1 <= static_cast<u32>(RenderSessionOperation::Export)) {
                const auto path_size = request_packet->GetPacketDataSize() - 5 * sizeof(u32);
                const std::string path{
                    reinterpret_cast<const char*>(packet_data.data() + 5 * sizeof(u32)), path_size};
                HandleRenderSession(*request_packet, operation,
                                    static_cast<u64>(id_low) | static_cast<u64>(id_high) << 32,
                                    start, count, path);
                success = true;
            }
            break;
        }
        case PacketType::RenderOutput: {
            const auto operation = static_cast<RenderOutputOperation>(arg1);
            const bool valid_size = (operation == RenderOutputOperation::Status &&
                                     request_packet->GetPacketDataSize() == 4 * sizeof(u32)) ||
                                    (operation == RenderOutputOperation::Read &&
                                     request_packet->GetPacketDataSize() == 6 * sizeof(u32));
            if (valid_size) {
                HandleRenderOutput(*request_packet, operation,
                                   static_cast<u64>(arg2) | static_cast<u64>(read_arg(2)) << 32,
                                   read_arg(3), read_arg(4), read_arg(5));
                success = true;
            }
            break;
        }
        case PacketType::RenderDebug: {
            const auto operation = static_cast<RenderDebugOperation>(arg1);
            if (arg1 <= static_cast<u32>(RenderDebugOperation::DrawBreak)) {
                HandleRenderDebug(*request_packet, operation,
                                  packet_data.first(request_packet->GetPacketDataSize()));
                success = true;
            }
            break;
        }
        case PacketType::PicaTrace: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            const u32 arg5 = read_arg(4, UINT32_MAX);
            if (arg1 <= static_cast<u32>(PicaTraceOperation::ReadWrites) &&
                arg4 <= MAX_PACKET_DATA_SIZE) {
                HandlePicaTrace(*request_packet, static_cast<PicaTraceOperation>(arg1), arg2, arg3,
                                arg4, arg5);
                success = true;
            }
            break;
        }
        case PacketType::CPURegisters: {
            const u32 arg3 = read_arg(2, 1);
            if (request_packet->GetPacketDataSize() >= 4 * sizeof(u32)) {
                HandleCPURegisters(*request_packet, arg1, arg2, arg3, read_arg(3));
            } else {
                HandleCPURegisters(*request_packet, 0, arg1, arg2, arg3);
            }
            success = true;
            break;
        }
        case PacketType::GXCommandTrace: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3, UINT32_MAX);
            if (arg1 <= static_cast<u32>(GXCommandTraceOperation::Clear)) {
                HandleGXCommandTrace(*request_packet, static_cast<GXCommandTraceOperation>(arg1),
                                     arg2, arg3, arg4);
                success = true;
            }
            break;
        }
        case PacketType::PicaShader: {
            const u32 arg3 = read_arg(2);
            const u32 arg4 = read_arg(3);
            const u32 arg5 = read_arg(4, UINT32_MAX);
            if (arg1 <= static_cast<u32>(PicaShaderOperation::Clear)) {
                HandlePicaShader(*request_packet, static_cast<PicaShaderOperation>(arg1), arg2,
                                 arg3, arg4, arg5);
                success = true;
            }
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
