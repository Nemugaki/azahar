// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <nihstro/bit_field.h>
#include <nihstro/float24.h>
#include <nihstro/shader_binary.h>
#include "common/assert.h"
#include "common/bit_field.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/pica/regs_shader.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_base.h"
#include "video_core/shader/shader_interpreter.h"

using nihstro::DVLBHeader;
using nihstro::DVLEHeader;
using nihstro::DVLPHeader;

namespace Pica {

DebugContext::DebugContext()
    : render_sessions{std::make_shared<Debugger::RenderSessionManager>(
          std::string{"Azahar "} + Common::g_scm_rev + " " + Common::g_scm_desc, "Pica")} {
    Debugger::CaptureLimits limits;
    limits.owned_bytes = static_cast<u64>(Settings::values.debugger_cache_mb.GetValue()) << 20;
    GetRenderSession()->SetCaptureLimits(limits);
}

void DebugContext::DoOnEvent(Event event, const void* data) {
    std::unique_lock observer_lock{observer_mutex};
    {
        std::unique_lock lock{breakpoint_mutex};

        // Commit the rasterizer's caches so framebuffers, render targets, etc. will show on debug
        // widgets
        Core::System::GetInstance().GPU().Renderer().Rasterizer()->FlushAll();

        // TODO: Should stop the CPU thread here once we multithread emulation.

        active_breakpoint = event;
        at_breakpoint = true;
        vertex_input_valid = event == Event::VertexShaderInvocation && data;
        if (vertex_input_valid) {
            std::memcpy(std::addressof(vertex_input), data, sizeof(vertex_input));
        }
        Core::System::GetInstance().SetDebugState(Core::DebugPauseReason::Pica,
                                                  static_cast<u32>(event));
    }

    for (auto& breakpoint_observer : breakpoint_observers) {
        breakpoint_observer->OnPicaBreakPointHit(event, data);
    }
    observer_lock.unlock();

    {
        std::unique_lock lock{breakpoint_mutex};
        // Wait until another thread tells us to Resume()
        resume_from_breakpoint.wait(lock, [&] { return !at_breakpoint; });
    }
}

void DebugContext::OnDraw(const Debugger::DrawInfo& info) {
    if (auto session = GetRenderSession(); session->IsCaptureEnabled()) {
        session->RecordDraw(info);
    }
    OnEvent(Event::IncomingPrimitiveBatch, &info);
}

void DebugContext::OnDraw(const Debugger::DrawInfo& info, const Debugger::Shader& shader,
                          std::span<const Debugger::ResourceView> resources,
                          std::span<const u32, 0x300> registers) {
    if (auto session = GetRenderSession(); session->IsCaptureEnabled()) {
        session->RecordDraw(info, shader, resources, registers);
    }
    OnEvent(Event::IncomingPrimitiveBatch, &info);
}

void DebugContext::OnRegisterWrite(Debugger::RegisterWrite write) {
    if (auto session = GetRenderSession(); session->IsCaptureEnabled()) {
        session->RecordRegisterWrite(write);
    }
}

void DebugContext::ArmDrawBreak(u32 frame_draw, std::optional<u32> color_address) {
    draw_break_frame_draw = frame_draw;
    draw_break_color = color_address.value_or(0);
    draw_break_filter_color = color_address.has_value();
    draw_break_phase = 1;
    GetRenderSession()->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::RPC, true);
}

void DebugContext::CancelDrawBreak() {
    draw_break_phase = 0;
    GetRenderSession()->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::RPC, false);
}

DebugContext::DrawBreakState DebugContext::GetDrawBreakState() const {
    return {draw_break_phase, draw_break_frame_draw, draw_break_color, draw_break_filter_color};
}

bool DebugContext::HandleDrawBreak(Event event) {
    const u32 phase = draw_break_phase;
    if (phase == 1 && event == Event::IncomingPrimitiveBatch) {
        const auto session = GetRenderSession();
        const auto position = session->GetPosition();
        const auto target = session->GetRenderTarget();
        if (position.frame_draw != draw_break_frame_draw + 1 ||
            (draw_break_filter_color && target.color_address != draw_break_color)) {
            return false;
        }
        draw_break_phase = 2;
        DoOnEvent(event, nullptr);
        return true;
    }
    if (phase == 2 && event == Event::FinishedPrimitiveBatch) {
        draw_break_phase = 3;
        DoOnEvent(event, nullptr);
        GetRenderSession()->SetOutputCaptureEnabled(Debugger::RenderSession::CaptureOwner::RPC,
                                                    false);
        return true;
    }
    return false;
}

void DebugContext::Resume() {
    {
        std::lock_guard lock{breakpoint_mutex};
        at_breakpoint = false;
    }

    Core::System::GetInstance().SetDebugState(Core::DebugPauseReason::Running);

    {
        std::lock_guard lock{observer_mutex};
        for (auto& breakpoint_observer : breakpoint_observers) {
            breakpoint_observer->OnPicaResume();
        }
    }

    resume_from_breakpoint.notify_one();
}

void DebugContext::ResumeUntilFrame() {
    ignore_breakpoints_until_frame = true;
    Resume();
    Core::System::GetInstance().SetDebugState(Core::DebugPauseReason::FrameAdvance);
}

void DebugContext::OnVBlank() {
    ignore_breakpoints_until_frame = false;
}

void DebugContext::OnFramePresented() {
    auto session = GetRenderSession();
    Debugger::CaptureLimits limits;
    limits.owned_bytes = static_cast<u64>(Settings::values.debugger_cache_mb.GetValue()) << 20;
    session->SetCaptureLimits(limits);
    session->RecordFrame();
}

void DebugContext::SetBreakpointCondition(Event event, BreakPointCondition condition) {
    std::lock_guard lock{breakpoint_mutex};
    breakpoint_conditions[static_cast<int>(event)] = condition;
}

DebugContext::BreakPointCondition DebugContext::GetBreakpointCondition(Event event) {
    std::lock_guard lock{breakpoint_mutex};
    return breakpoint_conditions[static_cast<int>(event)];
}

void DebugContext::SetBreakpointOptions(Event event, bool one_shot, u32 skip_count) {
    auto& breakpoint = breakpoints[static_cast<int>(event)];
    breakpoint.one_shot = one_shot;
    breakpoint.skip_remaining = skip_count;
    breakpoint.hit_count = 0;
}

DebugContext::BreakPointOptions DebugContext::GetBreakpointOptions(Event event) const {
    const auto& breakpoint = breakpoints[static_cast<int>(event)];
    return {breakpoint.one_shot, breakpoint.skip_remaining, breakpoint.hit_count};
}

bool DebugContext::MatchesCondition(Event event, const void* data) {
    const auto condition = GetBreakpointCondition(event);
    if (condition.field == ConditionField::None) {
        return true;
    }

    u32 actual{};
    const auto session = GetRenderSession();
    const auto target = session->GetRenderTarget();
    const auto position = session->GetPosition();
    switch (condition.field) {
    case ConditionField::EventData:
        if (!data || (event != Event::PicaCommandLoaded && event != Event::PicaCommandProcessed)) {
            return false;
        }
        std::memcpy(&actual, data, sizeof(actual));
        break;
    case ConditionField::ColorBuffer:
        actual = target.color_address;
        break;
    case ConditionField::DepthBuffer:
        actual = target.depth_address;
        break;
    case ConditionField::DrawIndex:
        actual = position.draw;
        break;
    case ConditionField::FrameIndex:
        actual = position.frame;
        break;
    case ConditionField::None:
        return true;
    }
    return (actual & condition.mask) == (condition.value & condition.mask);
}

void DebugContext::SetRenderTargetInfo(Debugger::RenderTarget info) {
    GetRenderSession()->SetRenderTarget(info);
}

DebugContext::BreakPointState DebugContext::GetBreakpointState() {
    std::lock_guard lock{breakpoint_mutex};
    u32 enabled_mask = 0;
    for (u32 i = 0; i < breakpoints.size(); ++i) {
        enabled_mask |= static_cast<u32>(breakpoints[i].enabled) << i;
    }
    return {enabled_mask, active_breakpoint, at_breakpoint};
}

std::optional<AttributeBuffer> DebugContext::GetVertexInput() {
    std::lock_guard lock{breakpoint_mutex};
    if (!at_breakpoint || !vertex_input_valid) {
        return std::nullopt;
    }
    return vertex_input;
}

std::shared_ptr<DebugContext> g_debug_context; // TODO: Get rid of this global

namespace DebugUtils {

VertexShaderSnapshot CaptureVertexShader(
    const ShaderRegs& config, ShaderSetup& setup,
    const RasterizerRegs::VSOutputAttributes* output_attributes, const AttributeBuffer& input) {
    VertexShaderSnapshot snapshot;
    const auto& program = setup.GetProgramCode();
    const auto& swizzles = setup.GetSwizzleData();
    snapshot.program.assign(program.begin(), program.end());
    snapshot.swizzles.assign(swizzles.begin(), swizzles.end());
    snapshot.binary = BuildShaderBinary(config, setup, output_attributes);
    snapshot.entry_point = config.main_offset;
    snapshot.input_count = config.max_input_attribute_index + 1;
    for (u32 attribute = 0; attribute < snapshot.input_count; ++attribute) {
        snapshot.input_mapping[attribute] = config.GetRegisterForAttribute(attribute);
    }

    Shader::InterpreterEngine engine;
    engine.SetupBatch(setup, snapshot.entry_point);
    snapshot.cycles = engine.ProduceDebugInfo(setup, input, config);
    return snapshot;
}

std::vector<u8> BuildShaderBinary(const ShaderRegs& config, const ShaderSetup& setup,
                                  const RasterizerRegs::VSOutputAttributes* output_attributes) {
    struct StuffToWrite {
        const u8* pointer;
        u32 size;
    };
    std::vector<StuffToWrite> writing_queue;
    u32 write_offset = 0;

    auto QueueForWriting = [&writing_queue, &write_offset](const u8* pointer, u32 size) {
        writing_queue.push_back({pointer, size});
        u32 old_write_offset = write_offset;
        write_offset += size;
        return old_write_offset;
    };

    // First off, try to translate Pica state (one enum for output attribute type and component)
    // into shbin format (separate type and component mask).
    union OutputRegisterInfo {
        enum Type : u64 {
            POSITION = 0,
            QUATERNION = 1,
            COLOR = 2,
            TEXCOORD0 = 3,
            TEXCOORD1 = 5,
            TEXCOORD2 = 6,

            VIEW = 8,
        };

        BitField<0, 64, u64> hex;

        BitField<0, 16, Type> type;
        BitField<16, 16, u64> id;
        BitField<32, 4, u64> component_mask;
    };

    // This is put into a try-catch block to make sure we notice unknown configurations.
    std::vector<OutputRegisterInfo> output_info_table;
    for (unsigned i = 0; i < 7; ++i) {
        using OutputAttributes = Pica::RasterizerRegs::VSOutputAttributes;

        // TODO: It's still unclear how the attribute components map to the register!
        //       Once we know that, this code probably will not make much sense anymore.
        std::map<OutputAttributes::Semantic, std::pair<OutputRegisterInfo::Type, u32>> map = {
            {OutputAttributes::POSITION_X, {OutputRegisterInfo::POSITION, 1}},
            {OutputAttributes::POSITION_Y, {OutputRegisterInfo::POSITION, 2}},
            {OutputAttributes::POSITION_Z, {OutputRegisterInfo::POSITION, 4}},
            {OutputAttributes::POSITION_W, {OutputRegisterInfo::POSITION, 8}},
            {OutputAttributes::QUATERNION_X, {OutputRegisterInfo::QUATERNION, 1}},
            {OutputAttributes::QUATERNION_Y, {OutputRegisterInfo::QUATERNION, 2}},
            {OutputAttributes::QUATERNION_Z, {OutputRegisterInfo::QUATERNION, 4}},
            {OutputAttributes::QUATERNION_W, {OutputRegisterInfo::QUATERNION, 8}},
            {OutputAttributes::COLOR_R, {OutputRegisterInfo::COLOR, 1}},
            {OutputAttributes::COLOR_G, {OutputRegisterInfo::COLOR, 2}},
            {OutputAttributes::COLOR_B, {OutputRegisterInfo::COLOR, 4}},
            {OutputAttributes::COLOR_A, {OutputRegisterInfo::COLOR, 8}},
            {OutputAttributes::TEXCOORD0_U, {OutputRegisterInfo::TEXCOORD0, 1}},
            {OutputAttributes::TEXCOORD0_V, {OutputRegisterInfo::TEXCOORD0, 2}},
            {OutputAttributes::TEXCOORD1_U, {OutputRegisterInfo::TEXCOORD1, 1}},
            {OutputAttributes::TEXCOORD1_V, {OutputRegisterInfo::TEXCOORD1, 2}},
            {OutputAttributes::TEXCOORD2_U, {OutputRegisterInfo::TEXCOORD2, 1}},
            {OutputAttributes::TEXCOORD2_V, {OutputRegisterInfo::TEXCOORD2, 2}},
            {OutputAttributes::VIEW_X, {OutputRegisterInfo::VIEW, 1}},
            {OutputAttributes::VIEW_Y, {OutputRegisterInfo::VIEW, 2}},
            {OutputAttributes::VIEW_Z, {OutputRegisterInfo::VIEW, 4}},
        };

        for (const auto& semantic : std::vector<OutputAttributes::Semantic>{
                 output_attributes[i].map_x, output_attributes[i].map_y, output_attributes[i].map_z,
                 output_attributes[i].map_w}) {
            if (semantic == OutputAttributes::INVALID)
                continue;

            try {
                OutputRegisterInfo::Type type = map.at(semantic).first;
                u32 component_mask = map.at(semantic).second;

                auto it = std::find_if(output_info_table.begin(), output_info_table.end(),
                                       [&i, &type](const OutputRegisterInfo& info) {
                                           return info.id == i && info.type == type;
                                       });

                if (it == output_info_table.end()) {
                    output_info_table.emplace_back();
                    output_info_table.back().type.Assign(type);
                    output_info_table.back().component_mask.Assign(component_mask);
                    output_info_table.back().id.Assign(i);
                } else {
                    it->component_mask.Assign(it->component_mask | component_mask);
                }
            } catch (const std::out_of_range&) {
                DEBUG_ASSERT_MSG(false, "Unknown output attribute mapping");
                LOG_ERROR(HW_GPU,
                          "Unknown output attribute mapping: {:03x}, {:03x}, {:03x}, {:03x}",
                          (int)output_attributes[i].map_x.Value(),
                          (int)output_attributes[i].map_y.Value(),
                          (int)output_attributes[i].map_z.Value(),
                          (int)output_attributes[i].map_w.Value());
            }
        }
    }

    struct {
        DVLBHeader header;
        u32 dvle_offset;
    } dvlb{{DVLBHeader::MAGIC_WORD, 1}}; // 1 DVLE

    DVLPHeader dvlp{DVLPHeader::MAGIC_WORD};
    DVLEHeader dvle{DVLEHeader::MAGIC_WORD};

    QueueForWriting(reinterpret_cast<const u8*>(&dvlb), sizeof(dvlb));
    u32 dvlp_offset = QueueForWriting(reinterpret_cast<const u8*>(&dvlp), sizeof(dvlp));
    dvlb.dvle_offset = QueueForWriting(reinterpret_cast<const u8*>(&dvle), sizeof(dvle));

    const auto& program_code = setup.GetProgramCode();
    const u32 program_size = setup.GetBiggestProgramSize();
    dvlp.binary_offset = write_offset - dvlp_offset;
    dvlp.binary_size_words = program_size;
    QueueForWriting(reinterpret_cast<const u8*>(program_code.data()), program_size * sizeof(u32));

    const auto& swizzle_data = setup.GetSwizzleData();
    const u32 swizzle_size = setup.GetBiggestSwizzleSize();
    dvlp.swizzle_info_offset = write_offset - dvlp_offset;
    dvlp.swizzle_info_num_entries = swizzle_size;
    u32 dummy = 0;
    for (u32 i = 0; i < swizzle_size; ++i) {
        QueueForWriting(reinterpret_cast<const u8*>(&swizzle_data[i]), sizeof(swizzle_data[i]));
        QueueForWriting(reinterpret_cast<const u8*>(&dummy), sizeof(dummy));
    }

    dvle.main_offset_words = config.main_offset;
    dvle.output_register_table_offset = write_offset - dvlb.dvle_offset;
    dvle.output_register_table_size = static_cast<u32>(output_info_table.size());
    QueueForWriting(reinterpret_cast<const u8*>(output_info_table.data()),
                    static_cast<u32>(output_info_table.size() * sizeof(OutputRegisterInfo)));

    // TODO: Create a label table for "main"

    std::vector<nihstro::ConstantInfo> constant_table;
    for (unsigned i = 0; i < setup.uniforms.b.size(); ++i) {
        nihstro::ConstantInfo constant;
        std::memset(&constant, 0, sizeof(constant));
        constant.type = nihstro::ConstantInfo::Bool;
        constant.regid = i;
        constant.b = setup.uniforms.b[i];
        constant_table.emplace_back(constant);
    }
    for (unsigned i = 0; i < setup.uniforms.i.size(); ++i) {
        nihstro::ConstantInfo constant;
        std::memset(&constant, 0, sizeof(constant));
        constant.type = nihstro::ConstantInfo::Int;
        constant.regid = i;
        constant.i.x = setup.uniforms.i[i].x;
        constant.i.y = setup.uniforms.i[i].y;
        constant.i.z = setup.uniforms.i[i].z;
        constant.i.w = setup.uniforms.i[i].w;
        constant_table.emplace_back(constant);
    }
    for (unsigned i = 0; i < sizeof(setup.uniforms.f) / sizeof(setup.uniforms.f[0]); ++i) {
        nihstro::ConstantInfo constant;
        std::memset(&constant, 0, sizeof(constant));
        constant.type = nihstro::ConstantInfo::Float;
        constant.regid = i;
        constant.f.x = nihstro::to_float24(setup.uniforms.f[i].x.ToFloat32());
        constant.f.y = nihstro::to_float24(setup.uniforms.f[i].y.ToFloat32());
        constant.f.z = nihstro::to_float24(setup.uniforms.f[i].z.ToFloat32());
        constant.f.w = nihstro::to_float24(setup.uniforms.f[i].w.ToFloat32());

        // Store constant if it's different from zero..
        if (setup.uniforms.f[i].x.ToFloat32() != 0.0 || setup.uniforms.f[i].y.ToFloat32() != 0.0 ||
            setup.uniforms.f[i].z.ToFloat32() != 0.0 || setup.uniforms.f[i].w.ToFloat32() != 0.0)
            constant_table.emplace_back(constant);
    }
    dvle.constant_table_offset = write_offset - dvlb.dvle_offset;
    dvle.constant_table_size = static_cast<uint32_t>(constant_table.size());
    for (const auto& constant : constant_table) {
        QueueForWriting(reinterpret_cast<const u8*>(&constant), sizeof(constant));
    }

    std::vector<u8> result;
    result.reserve(write_offset);
    for (const auto& chunk : writing_queue) {
        result.insert(result.end(), chunk.pointer, chunk.pointer + chunk.size);
    }
    return result;
}

void DumpShader(const std::string& filename, const ShaderRegs& config, const ShaderSetup& setup,
                const RasterizerRegs::VSOutputAttributes* output_attributes) {
    const auto data = BuildShaderBinary(config, setup, output_attributes);
    std::ofstream file(filename, std::ios_base::out | std::ios_base::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

static std::unique_ptr<PicaTrace> pica_trace;
static std::mutex pica_trace_mutex;
static std::optional<PicaTraceOwner> pica_trace_owner;
std::atomic_bool g_is_pica_tracing = false;

bool StartPicaTracing(PicaTraceOwner owner) {
    std::lock_guard lock(pica_trace_mutex);
    if (pica_trace_owner) {
        LOG_WARNING(HW_GPU, "StartPicaTracing called even though tracing already running!");
        return false;
    }

    pica_trace = std::make_unique<PicaTrace>();
    pica_trace_owner = owner;
    g_is_pica_tracing = true;
    return true;
}

bool IsPicaTracing(PicaTraceOwner owner) {
    std::lock_guard lock(pica_trace_mutex);
    return pica_trace_owner == owner;
}

void OnPicaRegWrite(u16 cmd_id, u16 mask, u32 value) {
    if (g_debug_context) {
        g_debug_context->OnRegisterWrite({0, cmd_id, value, mask});
    }
    if (!g_is_pica_tracing) [[likely]]
        return;

    std::lock_guard lock(pica_trace_mutex);
    if (pica_trace) {
        const std::size_t max_writes =
            static_cast<std::size_t>(Settings::values.debugger_cache_mb.GetValue()) * 1024 * 1024 /
            sizeof(PicaTrace::Write);
        if (pica_trace->writes.size() < max_writes) {
            pica_trace->writes.push_back(PicaTrace::Write{cmd_id, mask, value});
        } else {
            pica_trace->truncated = true;
        }
    }
}

std::unique_ptr<PicaTrace> FinishPicaTracing(PicaTraceOwner owner) {
    std::lock_guard lock(pica_trace_mutex);
    if (pica_trace_owner != owner) {
        LOG_WARNING(HW_GPU, "FinishPicaTracing called by a non-owner");
        return {};
    }
    g_is_pica_tracing = false;
    pica_trace_owner.reset();
    std::unique_ptr<PicaTrace> ret(std::move(pica_trace));

    return ret;
}

} // namespace DebugUtils

} // namespace Pica
