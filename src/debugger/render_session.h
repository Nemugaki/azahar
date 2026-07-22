// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Debugger {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum CaptureCapability : u64 {
    Timeline = 1ULL << 0,
    RegisterWrites = 1ULL << 1,
    Resources = 1ULL << 2,
    Shaders = 1ULL << 3,
    Previews = 1ULL << 4,
};

struct RenderTarget {
    u32 color_address{};
    u32 depth_address{};
    u32 width{};
    u32 height{};
    u32 color_format{};
    u32 depth_format{};
};

enum class TimelineKind : u32 { Draw = 0, Frame = 1 };
enum class DrawMode : u32 { Arrays = 0, Indexed = 1, Immediate = 2 };

struct DrawInfo {
    DrawMode mode{};
    u32 vertex_count{};
    u32 topology{};
    u32 vertex_offset{};
    u32 vertex_shader_entry{};
};

struct TimelineEntry {
    u32 sequence{};
    TimelineKind kind{};
    u32 frame{};
    u32 draw{};
    u32 changed_mask{};
    RenderTarget target{};
    DrawInfo draw_info{};
};

struct TimelinePosition {
    u32 frame{};
    u32 draw{};
};

struct TimelineStatus {
    u32 count{};
    u32 oldest_sequence{};
    u32 newest_sequence{};
    bool truncated{};
};

struct TimelineQuery {
    u32 start{};
    u32 count{};
    TimelineKind kind{};
    bool filter_kind{};
    u32 required_changes{};
    u32 target_address{std::numeric_limits<u32>::max()};
    u32 shader_entry{std::numeric_limits<u32>::max()};
    u32 frame{std::numeric_limits<u32>::max()};
};

struct Capture {
    std::string producer;
    std::string backend;
    u64 capabilities{CaptureCapability::Timeline};
    bool complete{true};
    bool truncated{};
    std::string gap_reason;
    std::vector<TimelineEntry> timeline;
};

struct CaptureLimits {
    u64 total_bytes{1ULL << 30};
    u64 record_bytes{256ULL << 20};
    std::size_t timeline_entries{1'000'000};
};

/// Thread-safe, bounded render metadata shared by live producers and debugger clients.
class RenderSession {
public:
    static constexpr u32 Latest = std::numeric_limits<u32>::max();
    static constexpr std::size_t MaxEntries = 4096;

    void SetRenderTarget(RenderTarget target);
    RenderTarget GetRenderTarget() const;
    void RecordDraw(const DrawInfo& info);
    void RecordFrame();
    std::vector<TimelineEntry> Query(const TimelineQuery& query) const;
    TimelineStatus GetStatus() const;
    TimelinePosition GetPosition() const;
    void Clear();
    void SetFrameLimit(u32 frame_limit);
    bool Replace(std::vector<TimelineEntry> entries, bool was_truncated);

private:
    void Record(TimelineKind kind, const DrawInfo& info);
    void TrimFrames();

    mutable std::mutex mutex;
    RenderTarget render_target{};
    RenderTarget previous_target{};
    std::deque<TimelineEntry> timeline;
    u32 sequence{};
    u32 frame_index{};
    u32 draw_index{};
    u32 frame_limit{8};
    bool truncated{};
};

struct SessionDescriptor {
    u64 id{};
    std::string producer;
    std::string backend;
    u64 capabilities{};
    bool live{};
    bool complete{true};
    bool truncated{};
};

/// Owns the live producer session and a small set of immutable imported sessions.
class RenderSessionManager {
public:
    static constexpr u64 LiveSessionId = 1;
    static constexpr std::size_t MaxImportedSessions = 16;

    RenderSessionManager();
    std::shared_ptr<RenderSession> GetLive() const;
    std::shared_ptr<RenderSession> GetActive() const;
    u64 GetActiveId() const;
    std::vector<SessionDescriptor> List() const;
    u64 AddImported(Capture capture);
    bool Snapshot(u64 id, Capture& capture) const;
    bool Select(u64 id);
    bool Remove(u64 id);

private:
    struct StoredSession {
        SessionDescriptor descriptor;
        std::shared_ptr<RenderSession> session;
        std::string gap_reason;
    };

    mutable std::mutex sessions_mutex;
    std::vector<StoredSession> sessions;
    u64 active_id{LiveSessionId};
    u64 next_id{LiveSessionId + 1};
};

} // namespace Debugger
