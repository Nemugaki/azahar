// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Debugger {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

enum CaptureCapability : u64 {
    Timeline = 1ULL << 0,
    RegisterWrites = 1ULL << 1,
    Resources = 1ULL << 2,
    Shaders = 1ULL << 3,
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

enum class ResourceRole : u32 {
    ColorTarget,
    DepthTarget,
    IndexBuffer,
    VertexBuffer,
    Texture,
    Other,
};

struct ResourceView {
    ResourceRole role{ResourceRole::Other};
    u32 slot{};
    u64 address{};
    u32 format{};
    u32 width{};
    u32 height{};
    u32 stride{};
    std::span<const u8> bytes;
};

struct Resource {
    u64 id{};
    ResourceRole role{ResourceRole::Other};
    u64 address{};
    u32 format{};
    u32 width{};
    u32 height{};
    u32 stride{};
    std::vector<u8> bytes;
};

struct ResourceReference {
    ResourceRole role{ResourceRole::Other};
    u32 slot{};
    u64 resource_id{};
};

struct RegisterWrite {
    u32 bank{};
    u32 index{};
    u32 value{};
    u32 mask{};
};

struct Shader {
    u64 id{};
    u32 stage{};
    u32 entry_point{};
    std::vector<u32> code;
    std::vector<u32> metadata;
    std::vector<u8> state;
};

struct DrawCapture {
    TimelineEntry timeline{};
    u32 write_begin{};
    u32 write_count{};
    u64 shader_id{};
    std::vector<ResourceReference> resources;
};

struct DrawDetails {
    u32 write_count{};
    u64 shader_id{};
    u32 resource_count{};
};

struct CaptureStatus {
    u64 capabilities{};
    bool complete{};
    bool truncated{};
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
    u64 owned_bytes{};
    std::vector<TimelineEntry> timeline;
    std::vector<RegisterWrite> writes;
    std::vector<DrawCapture> draws;
    std::vector<Shader> shaders;
    std::vector<Resource> resources;
};

struct CaptureLimits {
    u64 total_bytes{1ULL << 30};
    std::size_t timeline_entries{1'000'000};
    std::size_t register_writes{1U << 20};
    std::size_t draws{1U << 16};
    std::size_t shaders{1U << 16};
    std::size_t resources{1U << 16};
    u64 owned_bytes{1ULL << 30};
};

/// Thread-safe, bounded render metadata shared by live producers and debugger clients.
class RenderSession {
public:
    static constexpr u32 Latest = std::numeric_limits<u32>::max();
    static constexpr std::size_t MaxEntries = 4096;

    void SetRenderTarget(RenderTarget target);
    RenderTarget GetRenderTarget() const;
    void RecordDraw(const DrawInfo& info);
    void RecordDraw(const DrawInfo& info, const Shader& shader,
                    std::span<const ResourceView> resources);
    void RecordOutput(const ResourceView& output);
    void RecordRegisterWrite(RegisterWrite write);
    void RecordFrame();
    std::vector<TimelineEntry> Query(const TimelineQuery& query) const;
    TimelineStatus GetStatus() const;
    TimelinePosition GetPosition() const;
    void Clear();
    void SetFrameLimit(u32 frame_limit);
    void SetCaptureLimits(CaptureLimits limits);
    void SetCaptureEnabled(bool enabled);
    bool IsCaptureEnabled() const;
    void SetOutputCaptureEnabled(bool enabled);
    bool IsOutputCaptureEnabled() const;
    bool Replace(std::vector<TimelineEntry> entries, bool was_truncated);
    bool Replace(Capture capture);
    void Snapshot(Capture& capture) const;
    CaptureStatus GetCaptureStatus() const;
    std::optional<DrawDetails> GetDrawDetails(u32 sequence) const;
    std::optional<Resource> GetDrawOutput(
        u32 sequence, u64 offset = 0,
        u64 count = std::numeric_limits<u64>::max()) const;

private:
    TimelineEntry Record(TimelineKind kind, const DrawInfo& info);
    u64 StoreResource(const ResourceView& view);
    void ClearRichCapture(const char* reason);
    void PruneRichCapture();
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
    CaptureLimits capture_limits;
    u32 draw_write_begin{};
    bool rich_truncated{};
    bool capture_enabled{true};
    bool output_capture_enabled{};
    std::string rich_gap_reason;
    u64 capture_capabilities{CaptureCapability::Timeline};
    u64 owned_bytes{};
    std::vector<RegisterWrite> writes;
    std::vector<DrawCapture> draws;
    std::vector<Shader> shaders;
    std::vector<Resource> resources;
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
    std::shared_ptr<RenderSession> Get(u64 id) const;
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
    };

    mutable std::mutex sessions_mutex;
    std::vector<StoredSession> sessions;
    u64 active_id{LiveSessionId};
    u64 next_id{LiveSessionId + 1};
};

} // namespace Debugger
