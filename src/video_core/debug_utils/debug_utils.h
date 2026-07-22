// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "video_core/pica/output_vertex.h"
#include "video_core/pica/regs_rasterizer.h"
#include "video_core/shader/debug_data.h"

namespace CiTrace {
class Recorder;
}

namespace Pica {

struct ShaderRegs;
struct ShaderSetup;

class DebugContext {
public:
    enum class Event {
        FirstEvent = 0,

        PicaCommandLoaded = FirstEvent,
        PicaCommandProcessed,
        IncomingPrimitiveBatch,
        FinishedPrimitiveBatch,
        VertexShaderInvocation,
        IncomingDisplayTransfer,
        GSPCommandProcessed,
        BufferSwapped,

        NumEvents
    };

    /**
     * Inherit from this class to be notified of events registered to some debug context.
     * Most importantly this is used for our debugger GUI.
     *
     * To implement event handling, override the OnPicaBreakPointHit and OnPicaResume methods.
     * @warning All BreakPointObservers need to be on the same thread to guarantee thread-safe state
     * access
     * @todo Evaluate an alternative interface, in which there is only one managing observer and
     * multiple child observers running (by design) on the same thread.
     */
    class BreakPointObserver {
    public:
        /// Constructs the object such that it observes events of the given DebugContext.
        BreakPointObserver(std::shared_ptr<DebugContext> debug_context)
            : context_weak(debug_context) {
            std::unique_lock lock{debug_context->observer_mutex};
            debug_context->breakpoint_observers.push_back(this);
        }

        virtual ~BreakPointObserver() {
            auto context = context_weak.lock();
            if (context) {
                bool last_observer;
                {
                    std::unique_lock lock(context->observer_mutex);
                    context->breakpoint_observers.remove(this);
                    last_observer = context->breakpoint_observers.empty();
                }

                // Resume after releasing breakpoint_mutex: Resume locks the same mutex.
                if (last_observer) {
                    context->Resume();
                }
            }
        }

        /**
         * Action to perform when a breakpoint was reached.
         * @param event Type of event which triggered the breakpoint
         * @param data Optional data pointer (if unused, this is a nullptr)
         * @note This function will perform nothing unless it is overridden in the child class.
         */
        virtual void OnPicaBreakPointHit(Event event, const void* data) {}

        /**
         * Action to perform when emulation is resumed from a breakpoint.
         * @note This function will perform nothing unless it is overridden in the child class.
         */
        virtual void OnPicaResume() {}

    protected:
        /**
         * Weak context pointer. This need not be valid, so when requesting a shared_ptr via
         * context_weak.lock(), always compare the result against nullptr.
         */
        std::weak_ptr<DebugContext> context_weak;
    };

    /**
     * Simple structure defining a breakpoint state
     */
    struct BreakPoint {
        std::atomic_bool enabled = false;
    };

    struct BreakPointState {
        u32 enabled_mask;
        Event active;
        bool at_breakpoint;
    };

    enum class ConditionField : u32 {
        None = 0,
        EventData = 1,
        ColorBuffer = 2,
        DepthBuffer = 3,
        DrawIndex = 4,
        FrameIndex = 5,
    };

    struct BreakPointCondition {
        ConditionField field{};
        u32 value{};
        u32 mask{UINT32_MAX};
    };

    struct RenderTargetInfo {
        u32 color_address{};
        u32 depth_address{};
        u32 width{};
        u32 height{};
        u32 color_format{};
        u32 depth_format{};
    };

    enum class TimelineKind : u32 { Draw = 0, Frame = 1 };

    struct TimelineEntry {
        u32 sequence{};
        TimelineKind kind{};
        u32 frame{};
        u32 draw{};
        u32 changed_mask{};
        RenderTargetInfo target{};
    };

    struct TimelinePosition {
        u32 frame{};
        u32 draw{};
    };

    /**
     * Static constructor used to create a shared_ptr of a DebugContext.
     */
    static std::shared_ptr<DebugContext> Construct() {
        return std::shared_ptr<DebugContext>(new DebugContext);
    }

    /**
     * Used by the emulation core when a given event has happened. If a breakpoint has been set
     * for this event, OnEvent calls the event handlers of the registered breakpoint observers.
     * The current thread then is halted until Resume() is called from another thread (or until
     * emulation is stopped).
     * @param event Event which has happened
     * @param data Optional data pointer (pass nullptr if unused). Needs to remain valid until
     * Resume() is called.
     */
    void OnEvent(Event event, const void* data) {
        if (event == Event::IncomingPrimitiveBatch) {
            RecordTimeline(TimelineKind::Draw);
        }
        // This check is left in the header to allow the compiler to inline it.
        if (ignore_breakpoints_until_frame || !breakpoints[(int)event].enabled ||
            !MatchesCondition(event, data))
            return;
        // For the rest of event handling, call a separate function.
        DoOnEvent(event, data);
    }

    void DoOnEvent(Event event, const void* data);

    /**
     * Resume from the current breakpoint.
     * @warning Calling this from the same thread that OnEvent was called in will cause a deadlock.
     * Calling from any other thread is safe.
     */
    void Resume();

    /// Resume a GPU breakpoint without immediately stopping again before the next frame.
    void ResumeUntilFrame();

    /// Called by the GPU at the frame boundary to restore temporarily suppressed breakpoints.
    void OnFrameBoundary();

    void SetBreakpoint(Event event, bool enabled) {
        breakpoints[static_cast<int>(event)].enabled = enabled;
    }

    bool IsBreakpointEnabled(Event event) const {
        return breakpoints[static_cast<int>(event)].enabled;
    }

    void SetBreakpointCondition(Event event, BreakPointCondition condition);
    BreakPointCondition GetBreakpointCondition(Event event);
    void SetRenderTargetInfo(RenderTargetInfo info);
    RenderTargetInfo GetRenderTargetInfo() const;
    std::vector<TimelineEntry> GetTimeline(u32 start, u32 count, TimelineKind kind,
                                           bool filter_kind, u32 required_changes = 0) const;
    u32 GetTimelineCount() const;
    TimelinePosition GetTimelinePosition() const;
    void ClearTimeline();

    BreakPointState GetBreakpointState();
    std::optional<AttributeBuffer> GetVertexInput();

    /**
     * Delete all set breakpoints and resume emulation.
     */
    void ClearBreakpoints() {
        for (auto& bp : breakpoints) {
            bp.enabled = false;
        }
        breakpoint_conditions.fill({});
        ignore_breakpoints_until_frame = false;
        Resume();
    }

    // TODO: Evaluate if access to these members should be hidden behind a public interface.
    std::array<BreakPoint, (int)Event::NumEvents> breakpoints;
    Event active_breakpoint = Event::FirstEvent;
    bool at_breakpoint = false;

    std::shared_ptr<CiTrace::Recorder> recorder = nullptr;

private:
    bool MatchesCondition(Event event, const void* data);
    void RecordTimeline(TimelineKind kind);
    /**
     * Private default constructor to make sure people always construct this through Construct()
     * instead.
     */
    DebugContext() = default;

    /// Mutex protecting current breakpoint state.
    std::mutex breakpoint_mutex;

    /// Kept separate so observer UI work does not hold the breakpoint state mutex.
    std::mutex observer_mutex;

    /// Used by OnEvent to wait for resumption.
    std::condition_variable resume_from_breakpoint;

    /// List of registered observers
    std::list<BreakPointObserver*> breakpoint_observers;
    AttributeBuffer vertex_input{};
    bool vertex_input_valid{};
    std::atomic_bool ignore_breakpoints_until_frame = false;
    std::array<BreakPointCondition, static_cast<int>(Event::NumEvents)> breakpoint_conditions{};
    mutable std::mutex timeline_mutex;
    RenderTargetInfo render_target{};
    RenderTargetInfo previous_timeline_target{};
    std::deque<TimelineEntry> timeline;
    u32 timeline_sequence{};
    u32 frame_index{};
    u32 draw_index{};
};

extern std::shared_ptr<DebugContext> g_debug_context; // TODO: Get rid of this global

namespace DebugUtils {

struct VertexShaderSnapshot {
    std::vector<u32> program;
    std::vector<u32> swizzles;
    std::vector<u8> binary;
    Shader::DebugData<true> cycles;
    std::array<u32, 16> input_mapping{};
    u32 entry_point{};
    u32 input_count{};
};

VertexShaderSnapshot CaptureVertexShader(
    const ShaderRegs& config, ShaderSetup& setup,
    const RasterizerRegs::VSOutputAttributes* output_attributes, const AttributeBuffer& input);

void DumpShader(const std::string& filename, const ShaderRegs& config, const ShaderSetup& setup,
                const RasterizerRegs::VSOutputAttributes* output_attributes);
std::vector<u8> BuildShaderBinary(const ShaderRegs& config, const ShaderSetup& setup,
                                  const RasterizerRegs::VSOutputAttributes* output_attributes);

// Utility class to log Pica commands.
struct PicaTrace {
    struct Write {
        u16 cmd_id;
        u16 mask;
        u32 value;
    };
    std::vector<Write> writes;
};

extern std::atomic_bool g_is_pica_tracing;

void StartPicaTracing();
inline bool IsPicaTracing() {
    return g_is_pica_tracing;
}
void OnPicaRegWrite(u16 cmd_id, u16 mask, u32 value);
std::unique_ptr<PicaTrace> FinishPicaTracing();

} // namespace DebugUtils

} // namespace Pica
