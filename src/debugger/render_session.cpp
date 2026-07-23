// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "debugger/render_session.h"

#include <algorithm>
#include <cstring>

#include "common/scm_rev.h"

namespace Debugger {
namespace {

u64 ShaderBytes(const Shader& shader) {
    return static_cast<u64>(shader.code.size() + shader.metadata.size()) * sizeof(u32) +
           shader.state.size();
}

bool SameShader(const Shader& left, const Shader& right) {
    return left.stage == right.stage && left.entry_point == right.entry_point &&
           left.code == right.code && left.metadata == right.metadata && left.state == right.state;
}

bool SameResource(const Resource& resource, const ResourceView& view) {
    return resource.role == view.role && resource.address == view.address &&
           resource.format == view.format && resource.width == view.width &&
           resource.height == view.height && resource.stride == view.stride &&
           resource.tiling == view.tiling && resource.origin == view.origin &&
           resource.bytes.size() == view.bytes.size() &&
           std::equal(resource.bytes.begin(), resource.bytes.end(), view.bytes.begin());
}

template <typename T>
u64 NextId(const std::vector<T>& values) {
    u64 id{};
    for (const auto& value : values) {
        id = std::max(id, value.id);
    }
    return id + 1;
}

} // namespace

void RenderSession::SetRenderTarget(RenderTarget target) {
    std::lock_guard lock{mutex};
    render_target = target;
}

RenderTarget RenderSession::GetRenderTarget() const {
    std::lock_guard lock{mutex};
    return render_target;
}

void RenderSession::RecordDraw(const DrawInfo& info) {
    std::lock_guard lock{mutex};
    if (!capture_enabled) {
        return;
    }
    Record(TimelineKind::Draw, info);
    draw_write_begin = static_cast<u32>(writes.size());
}

void RenderSession::RecordDraw(const DrawInfo& info, const Shader& shader,
                               std::span<const ResourceView> resource_views,
                               std::span<const u32, 0x300> registers) {
    std::lock_guard lock{mutex};
    if (!capture_enabled) {
        return;
    }
    const auto entry = Record(TimelineKind::Draw, info);
    if (draws.size() >= capture_limits.draws) {
        rich_truncated = true;
        rich_gap_reason = "draw capture limit reached";
        gaps.push_back({entry.frame, entry.frame_draw, ResourceRole::Other, 0, rich_gap_reason});
        draw_write_begin = static_cast<u32>(writes.size());
        return;
    }

    DrawCapture draw{
        entry, draw_write_begin, static_cast<u32>(writes.size() - draw_write_begin), 0, {}, {}};
    std::ranges::copy(registers, draw.registers.begin());
    capture_capabilities |= CaptureCapability::RegisterState;
    draw_write_begin = static_cast<u32>(writes.size());

    const auto existing_shader = std::ranges::find_if(
        shaders, [&shader](const auto& value) { return SameShader(value, shader); });
    if (existing_shader != shaders.end()) {
        draw.shader_id = existing_shader->id;
    } else {
        const u64 size = ShaderBytes(shader);
        if (shaders.size() < capture_limits.shaders &&
            size <=
                capture_limits.owned_bytes - std::min(capture_limits.owned_bytes, owned_bytes)) {
            auto owned = shader;
            owned.id = NextId(shaders);
            draw.shader_id = owned.id;
            owned_bytes += size;
            shaders.push_back(std::move(owned));
            capture_capabilities |= CaptureCapability::Shaders;
        } else {
            rich_truncated = true;
            rich_gap_reason = "shader capture limit reached";
            gaps.push_back(
                {entry.frame, entry.frame_draw, ResourceRole::Other, 0, rich_gap_reason});
        }
    }

    for (const auto& view : resource_views) {
        if (const u64 id = StoreResource(view)) {
            draw.resources.push_back({view.role, view.slot, id, CapturePhase::DrawInput});
        } else {
            rich_truncated = true;
            gaps.push_back({entry.frame, entry.frame_draw, view.role, view.slot,
                            view.bytes.empty() ? "referenced resource is empty"
                                               : "resource capture limit reached"});
        }
    }
    draws.push_back(std::move(draw));
}

u64 RenderSession::StoreResource(const ResourceView& view) {
    const auto resource = std::ranges::find_if(
        resources, [&view](const auto& value) { return SameResource(value, view); });
    if (resource != resources.end()) {
        return resource->id;
    }
    if (resources.size() >= capture_limits.resources ||
        view.bytes.size() >
            capture_limits.owned_bytes - std::min(capture_limits.owned_bytes, owned_bytes)) {
        rich_truncated = true;
        rich_gap_reason = "resource capture limit reached";
        return 0;
    }
    Resource owned{NextId(resources), view.role,
                   view.address,      view.format,
                   view.width,        view.height,
                   view.stride,       view.tiling,
                   view.origin,       std::vector<u8>{view.bytes.begin(), view.bytes.end()}};
    const u64 id = owned.id;
    owned_bytes += owned.bytes.size();
    resources.push_back(std::move(owned));
    capture_capabilities |= CaptureCapability::Resources;
    return id;
}

void RenderSession::RecordOutput(const ResourceView& output, CapturePhase phase) {
    std::lock_guard lock{mutex};
    if (!capture_enabled || !output_capture_owners || draws.empty()) {
        return;
    }
    const u64 id = StoreResource(output);
    if (!id) {
        return;
    }
    auto& references = draws.back().resources;
    const auto existing = std::ranges::find_if(references, [phase](const auto& reference) {
        return reference.role == ResourceRole::ColorTarget && reference.phase == phase;
    });
    const ResourceReference reference{ResourceRole::ColorTarget, output.slot, id, phase};
    if (existing == references.end()) {
        references.push_back(reference);
    } else {
        *existing = reference;
    }
    capture_capabilities |= CaptureCapability::Outputs;
}

void RenderSession::RecordGap(ResourceRole role, u32 slot, std::string reason) {
    std::lock_guard lock{mutex};
    rich_truncated = true;
    gaps.push_back({frame_index, frame_draw_index, role, slot, std::move(reason)});
}

void RenderSession::RecordRegisterWrite(RegisterWrite write) {
    std::lock_guard lock{mutex};
    if (!capture_enabled) {
        return;
    }
    if (writes.size() >= capture_limits.register_writes) {
        rich_truncated = true;
        rich_gap_reason = "register write capture limit reached";
        gaps.push_back({frame_index, frame_draw_index, ResourceRole::Other, 0, rich_gap_reason});
        return;
    }
    writes.push_back(write);
    capture_capabilities |= CaptureCapability::RegisterWrites;
}

void RenderSession::RecordFrame() {
    std::lock_guard lock{mutex};
    if (!capture_enabled) {
        return;
    }
    Record(TimelineKind::Frame, {});
}

TimelineEntry RenderSession::Record(TimelineKind kind, const DrawInfo& info) {
    u32 changed{};
    changed |= render_target.color_address != previous_target.color_address ? 1U : 0U;
    changed |= render_target.depth_address != previous_target.depth_address ? 2U : 0U;
    changed |= render_target.width != previous_target.width ||
                       render_target.height != previous_target.height
                   ? 4U
                   : 0U;
    changed |= render_target.color_format != previous_target.color_format ||
                       render_target.depth_format != previous_target.depth_format
                   ? 8U
                   : 0U;
    previous_target = render_target;
    if (kind == TimelineKind::Frame) {
        ++frame_index;
        frame_draw_index = 0;
    }
    TimelineEntry entry{sequence++,       kind,    frame_index,   draw_index,
                        frame_draw_index, changed, render_target, info};
    if (kind == TimelineKind::Draw) {
        ++draw_index;
        ++frame_draw_index;
    }
    timeline.push_back(entry);
    if (timeline.size() > MaxEntries) {
        timeline.pop_front();
        truncated = true;
        PruneRichCapture();
    }
    if (kind == TimelineKind::Frame) {
        TrimFrames();
    }
    return entry;
}

std::vector<TimelineEntry> RenderSession::Query(const TimelineQuery& query) const {
    std::lock_guard lock{mutex};
    std::vector<TimelineEntry> result;
    if (query.count == 0) {
        return result;
    }
    result.reserve(std::min<std::size_t>(query.count, timeline.size()));
    const auto matches = [&query](const TimelineEntry& entry) {
        return (!query.filter_kind || entry.kind == query.kind) &&
               (entry.changed_mask & query.required_changes) == query.required_changes &&
               (query.target_address == Latest ||
                entry.target.color_address == query.target_address ||
                entry.target.depth_address == query.target_address) &&
               (query.shader_entry == Latest ||
                entry.draw_info.vertex_shader_entry == query.shader_entry) &&
               (query.frame == Latest || entry.frame == query.frame);
    };
    if (query.start == Latest) {
        for (auto entry = timeline.rbegin();
             entry != timeline.rend() && result.size() < query.count; ++entry) {
            if (matches(*entry)) {
                result.push_back(*entry);
            }
        }
        std::reverse(result.begin(), result.end());
        return result;
    }
    for (const auto& entry : timeline) {
        if (entry.sequence >= query.start && matches(entry)) {
            result.push_back(entry);
            if (result.size() == query.count) {
                break;
            }
        }
    }
    return result;
}

TimelineStatus RenderSession::GetStatus() const {
    std::lock_guard lock{mutex};
    return {static_cast<u32>(timeline.size()),
            timeline.empty() ? sequence : timeline.front().sequence,
            timeline.empty() ? sequence : timeline.back().sequence, truncated};
}

TimelinePosition RenderSession::GetPosition() const {
    std::lock_guard lock{mutex};
    return {frame_index, draw_index, frame_draw_index};
}

void RenderSession::ClearRichCapture(const char* reason) {
    writes.clear();
    draws.clear();
    shaders.clear();
    resources.clear();
    draw_write_begin = 0;
    owned_bytes = 0;
    capture_capabilities = CaptureCapability::Timeline;
    rich_truncated = true;
    rich_gap_reason = reason;
    gaps = {{frame_index, frame_draw_index, ResourceRole::Other, 0, reason}};
}

void RenderSession::Clear() {
    std::lock_guard lock{mutex};
    timeline.clear();
    previous_target = render_target;
    sequence = 0;
    frame_index = 0;
    draw_index = 0;
    frame_draw_index = 0;
    ++epoch;
    truncated = false;
    writes.clear();
    draws.clear();
    shaders.clear();
    resources.clear();
    draw_write_begin = 0;
    rich_truncated = false;
    rich_gap_reason.clear();
    gaps.clear();
    capture_capabilities = CaptureCapability::Timeline;
    owned_bytes = 0;
}

void RenderSession::SetFrameLimit(u32 limit) {
    std::lock_guard lock{mutex};
    frame_limit = std::max(1U, limit);
    TrimFrames();
}

void RenderSession::SetCaptureLimits(CaptureLimits limits) {
    std::lock_guard lock{mutex};
    capture_limits = limits;
    if (writes.size() > limits.register_writes || draws.size() > limits.draws ||
        shaders.size() > limits.shaders || resources.size() > limits.resources ||
        owned_bytes > limits.owned_bytes) {
        ClearRichCapture("rich capture reset after limit change");
    }
}

void RenderSession::SetCaptureEnabled(bool enabled) {
    std::lock_guard lock{mutex};
    capture_enabled = enabled;
}

bool RenderSession::IsCaptureEnabled() const {
    std::lock_guard lock{mutex};
    return capture_enabled;
}

void RenderSession::SetOutputCaptureEnabled(CaptureOwner owner, bool enabled) {
    std::lock_guard lock{mutex};
    const u32 bit = static_cast<u32>(owner);
    output_capture_owners = enabled ? output_capture_owners | bit : output_capture_owners & ~bit;
}

bool RenderSession::IsOutputCaptureEnabled() const {
    std::lock_guard lock{mutex};
    return output_capture_owners != 0;
}

bool RenderSession::Replace(std::vector<TimelineEntry> entries, bool was_truncated) {
    Capture capture;
    capture.timeline = std::move(entries);
    capture.truncated = was_truncated;
    return Replace(std::move(capture));
}

bool RenderSession::Replace(Capture capture) {
    for (std::size_t index = 0; index < capture.timeline.size(); ++index) {
        if (capture.timeline[index].kind > TimelineKind::Frame ||
            capture.timeline[index].draw_info.mode > DrawMode::Immediate ||
            (index && capture.timeline[index - 1].sequence >= capture.timeline[index].sequence)) {
            return false;
        }
    }

    std::lock_guard lock{mutex};
    timeline.assign(capture.timeline.begin(), capture.timeline.end());
    truncated = capture.truncated;
    capture_capabilities = capture.capabilities;
    rich_truncated = !capture.complete;
    rich_gap_reason = capture.gaps.empty() ? std::string{} : capture.gaps.front().reason;
    gaps = std::move(capture.gaps);
    owned_bytes = capture.owned_bytes;
    writes = std::move(capture.writes);
    shaders = std::move(capture.shaders);
    resources = std::move(capture.resources);
    draws.clear();
    for (auto& draw : capture.draws) {
        draws.push_back(std::move(draw));
    }
    draw_write_begin = static_cast<u32>(writes.size());
    if (timeline.empty()) {
        render_target = {};
        previous_target = {};
        sequence = 0;
        frame_index = 0;
        draw_index = 0;
        frame_draw_index = 0;
        ++epoch;
        return true;
    }
    const auto& last = timeline.back();
    render_target = last.target;
    previous_target = last.target;
    sequence = last.sequence == Latest ? Latest : last.sequence + 1;
    frame_index = last.frame;
    draw_index = last.draw;
    frame_draw_index = last.frame_draw + (last.kind == TimelineKind::Draw);
    ++epoch;
    return true;
}

void RenderSession::Snapshot(Capture& capture) const {
    std::lock_guard lock{mutex};
    capture.capabilities = capture_capabilities;
    capture.complete = !rich_truncated;
    capture.truncated = truncated || rich_truncated;
    capture.gaps = gaps;
    capture.owned_bytes = owned_bytes;
    capture.timeline.assign(timeline.begin(), timeline.end());
    capture.writes = writes;
    capture.draws = draws;
    capture.shaders = shaders;
    capture.resources = resources;
}

CaptureStatus RenderSession::GetCaptureStatus() const {
    std::lock_guard lock{mutex};
    return {capture_capabilities, !rich_truncated, truncated || rich_truncated};
}

std::optional<DrawDetails> RenderSession::GetDrawDetails(u32 sequence_value) const {
    std::lock_guard lock{mutex};
    const auto draw = std::ranges::find_if(draws, [sequence_value](const auto& value) {
        return value.timeline.sequence == sequence_value;
    });
    return draw == draws.end()
               ? std::nullopt
               : std::optional<DrawDetails>{{draw->write_count, draw->shader_id,
                                             static_cast<u32>(draw->resources.size())}};
}

std::optional<std::array<u32, 0x300>> RenderSession::GetDrawState(u32 sequence_value) const {
    std::lock_guard lock{mutex};
    const auto draw = std::ranges::find_if(draws, [sequence_value](const auto& value) {
        return value.timeline.sequence == sequence_value;
    });
    return draw == draws.end() ? std::nullopt
                               : std::optional<std::array<u32, 0x300>>{draw->registers};
}

std::optional<Resource> RenderSession::GetDrawOutput(u32 sequence_value, CapturePhase phase,
                                                     u64 offset, u64 count) const {
    std::lock_guard lock{mutex};
    const auto draw = std::ranges::find_if(draws, [sequence_value](const auto& value) {
        return value.timeline.sequence == sequence_value;
    });
    if (draw == draws.end()) {
        return std::nullopt;
    }
    const auto reference = std::ranges::find_if(draw->resources, [phase](const auto& value) {
        return value.role == ResourceRole::ColorTarget && value.phase == phase;
    });
    if (reference == draw->resources.end()) {
        return std::nullopt;
    }
    const auto resource = std::ranges::find_if(
        resources, [id = reference->resource_id](const auto& value) { return value.id == id; });
    if (resource == resources.end() || offset > resource->bytes.size()) {
        return std::nullopt;
    }
    const auto size =
        static_cast<std::size_t>(std::min<u64>(count, resource->bytes.size() - offset));
    const auto begin = resource->bytes.begin() + static_cast<std::size_t>(offset);
    Resource result{resource->id,     resource->role,       resource->address, resource->format,
                    resource->width,  resource->height,     resource->stride,  resource->tiling,
                    resource->origin, {begin, begin + size}};
    return result;
}

TimelinePage RenderSession::QueryPage(u64 requested_epoch, const TimelineQuery& query) const {
    std::lock_guard lock{mutex};
    TimelinePage page;
    page.epoch = epoch;
    if (requested_epoch && requested_epoch != epoch) {
        page.stale = true;
        return page;
    }
    if (!query.count) {
        return page;
    }
    const auto matches = [&query](const TimelineEntry& entry) {
        return (!query.filter_kind || entry.kind == query.kind) &&
               (entry.changed_mask & query.required_changes) == query.required_changes &&
               (query.target_address == Latest ||
                entry.target.color_address == query.target_address ||
                entry.target.depth_address == query.target_address) &&
               (query.shader_entry == Latest ||
                entry.draw_info.vertex_shader_entry == query.shader_entry) &&
               (query.frame == Latest || entry.frame == query.frame);
    };
    for (const auto& entry : timeline) {
        if (entry.sequence >= query.start && matches(entry)) {
            if (page.entries.size() == query.count) {
                page.has_more = true;
                break;
            }
            page.entries.push_back(entry);
            page.next_sequence = entry.sequence == Latest ? Latest : entry.sequence + 1;
        }
    }
    return page;
}

std::vector<CaptureIssue> RenderSession::Validate(bool require_outputs) const {
    std::lock_guard lock{mutex};
    std::vector<CaptureIssue> issues;
    if (truncated || rich_truncated) {
        issues.push_back({CaptureIssueKind::Truncated, frame_index, frame_draw_index,
                          ResourceRole::Other, 0, "capture was truncated"});
    }
    for (const auto& gap : gaps) {
        issues.push_back(
            {CaptureIssueKind::Gap, gap.frame, gap.draw, gap.role, gap.slot, gap.reason});
    }
    for (const auto& draw : draws) {
        if (!draw.shader_id) {
            issues.push_back({CaptureIssueKind::MissingShader, draw.timeline.frame,
                              draw.timeline.frame_draw, ResourceRole::Other, 0,
                              "draw has no captured shader"});
        }
        bool has_output = false;
        for (const auto& reference : draw.resources) {
            const auto resource =
                std::ranges::find_if(resources, [id = reference.resource_id](const auto& value) {
                    return value.id == id;
                });
            if (resource == resources.end()) {
                issues.push_back({CaptureIssueKind::InvalidReference, draw.timeline.frame,
                                  draw.timeline.frame_draw, reference.role, reference.slot,
                                  "resource reference is missing"});
            } else if (resource->bytes.empty()) {
                issues.push_back({CaptureIssueKind::EmptyResource, draw.timeline.frame,
                                  draw.timeline.frame_draw, reference.role, reference.slot,
                                  "referenced resource has no bytes"});
            }
            has_output |= reference.role == ResourceRole::ColorTarget &&
                          reference.phase == CapturePhase::PostDraw &&
                          resource != resources.end() && !resource->bytes.empty();
        }
        if (require_outputs && !has_output) {
            issues.push_back({CaptureIssueKind::MissingOutput, draw.timeline.frame,
                              draw.timeline.frame_draw, ResourceRole::ColorTarget, 0,
                              "draw has no post-draw output"});
        }
    }
    return issues;
}

void RenderSession::PruneRichCapture() {
    if (timeline.empty()) {
        writes.clear();
        draws.clear();
        shaders.clear();
        resources.clear();
        draw_write_begin = 0;
        owned_bytes = 0;
        capture_capabilities = CaptureCapability::Timeline;
        return;
    }

    const u32 first_sequence = timeline.front().sequence;
    std::erase_if(draws, [first_sequence](const auto& draw) {
        return draw.timeline.sequence < first_sequence;
    });
    if (draws.empty()) {
        const u32 removed_writes = std::min<u32>(draw_write_begin, writes.size());
        writes.erase(writes.begin(), writes.begin() + removed_writes);
        draw_write_begin = 0;
    } else {
        const u32 removed_writes = std::min<u32>(draws.front().write_begin, writes.size());
        writes.erase(writes.begin(), writes.begin() + removed_writes);
        for (auto& draw : draws) {
            draw.write_begin -= removed_writes;
        }
        draw_write_begin -= std::min(draw_write_begin, removed_writes);
    }

    std::erase_if(shaders, [this](const auto& shader) {
        return std::ranges::none_of(
            draws, [&shader](const auto& draw) { return draw.shader_id == shader.id; });
    });
    std::erase_if(resources, [this](const auto& resource) {
        return std::ranges::none_of(draws, [&resource](const auto& draw) {
            return std::ranges::any_of(draw.resources, [&resource](const auto& reference) {
                return reference.resource_id == resource.id;
            });
        });
    });
    owned_bytes = 0;
    for (const auto& shader : shaders) {
        owned_bytes += ShaderBytes(shader);
    }
    for (const auto& resource : resources) {
        owned_bytes += resource.bytes.size();
    }
    capture_capabilities = CaptureCapability::Timeline;
    if (!writes.empty()) {
        capture_capabilities |= CaptureCapability::RegisterWrites;
    }
    if (!shaders.empty()) {
        capture_capabilities |= CaptureCapability::Shaders;
    }
    if (!resources.empty()) {
        capture_capabilities |= CaptureCapability::Resources;
    }
    if (!draws.empty()) {
        capture_capabilities |= CaptureCapability::RegisterState;
    }
    if (std::ranges::any_of(draws, [](const auto& draw) {
            return std::ranges::any_of(draw.resources, [](const auto& reference) {
                return reference.phase != CapturePhase::DrawInput;
            });
        })) {
        capture_capabilities |= CaptureCapability::Outputs;
    }
}

void RenderSession::TrimFrames() {
    bool removed = false;
    while (!timeline.empty() && frame_index >= timeline.front().frame &&
           frame_index - timeline.front().frame >= frame_limit) {
        timeline.pop_front();
        truncated = true;
        removed = true;
    }
    if (removed) {
        PruneRichCapture();
    }
}

RenderSessionManager::RenderSessionManager() {
    const std::string producer =
        std::string{"Azahar "} + Common::g_scm_rev + " " + Common::g_scm_desc;
    sessions.push_back({{LiveSessionId, producer, "Pica", Timeline, true, true, false},
                        std::make_shared<RenderSession>()});
}

std::shared_ptr<RenderSession> RenderSessionManager::GetLive() const {
    std::lock_guard lock{sessions_mutex};
    return sessions.front().session;
}

std::shared_ptr<RenderSession> RenderSessionManager::Get(u64 id) const {
    std::lock_guard lock{sessions_mutex};
    const auto found = std::ranges::find_if(
        sessions, [id](const auto& stored) { return stored.descriptor.id == id; });
    return found == sessions.end() ? nullptr : found->session;
}

std::shared_ptr<RenderSession> RenderSessionManager::GetActive() const {
    std::lock_guard lock{sessions_mutex};
    const auto found = std::ranges::find_if(
        sessions, [this](const auto& stored) { return stored.descriptor.id == active_id; });
    return found == sessions.end() ? sessions.front().session : found->session;
}

u64 RenderSessionManager::GetActiveId() const {
    std::lock_guard lock{sessions_mutex};
    return active_id;
}

std::vector<SessionDescriptor> RenderSessionManager::List() const {
    std::lock_guard lock{sessions_mutex};
    std::vector<SessionDescriptor> result;
    result.reserve(sessions.size());
    for (const auto& stored : sessions) {
        auto descriptor = stored.descriptor;
        const auto status = stored.session->GetCaptureStatus();
        descriptor.capabilities = status.capabilities;
        descriptor.complete &= status.complete;
        descriptor.truncated |= status.truncated;
        result.push_back(std::move(descriptor));
    }
    return result;
}

u64 RenderSessionManager::AddImported(Capture capture) {
    const auto producer = capture.producer;
    const auto backend = capture.backend;
    const u64 capabilities = capture.capabilities;
    const bool complete = capture.complete;
    auto session = std::make_shared<RenderSession>();
    if (!session->Replace(std::move(capture))) {
        return 0;
    }
    const bool truncated = session->GetStatus().truncated;
    std::lock_guard lock{sessions_mutex};
    if (sessions.size() >= MaxImportedSessions + 1) {
        return 0;
    }
    SessionDescriptor descriptor{next_id++, producer, backend,  capabilities,
                                 false,     complete, truncated};
    sessions.push_back({std::move(descriptor), std::move(session)});
    active_id = sessions.back().descriptor.id;
    return active_id;
}

bool RenderSessionManager::Snapshot(u64 id, Capture& capture) const {
    std::shared_ptr<RenderSession> session;
    SessionDescriptor descriptor;
    {
        std::lock_guard lock{sessions_mutex};
        const auto found = std::ranges::find_if(
            sessions, [id](const auto& stored) { return stored.descriptor.id == id; });
        if (found == sessions.end()) {
            return false;
        }
        session = found->session;
        descriptor = found->descriptor;
    }
    session->Snapshot(capture);
    const auto issues = session->Validate(false);
    for (const auto& issue : issues) {
        if (issue.kind != CaptureIssueKind::Gap && issue.kind != CaptureIssueKind::Truncated) {
            capture.gaps.push_back(
                {issue.frame, issue.draw, issue.role, issue.slot, issue.message});
        }
    }
    capture.producer = descriptor.producer;
    capture.backend = descriptor.backend;
    capture.complete &= descriptor.complete && issues.empty();
    capture.truncated |= descriptor.truncated;
    return true;
}

bool RenderSessionManager::Select(u64 id) {
    std::lock_guard lock{sessions_mutex};
    const bool exists = std::ranges::any_of(
        sessions, [id](const auto& stored) { return stored.descriptor.id == id; });
    if (exists) {
        active_id = id;
    }
    return exists;
}

bool RenderSessionManager::Remove(u64 id) {
    if (id == LiveSessionId) {
        return false;
    }
    std::lock_guard lock{sessions_mutex};
    const auto found = std::ranges::find_if(
        sessions, [id](const auto& stored) { return stored.descriptor.id == id; });
    if (found == sessions.end()) {
        return false;
    }
    sessions.erase(found);
    if (active_id == id) {
        active_id = LiveSessionId;
    }
    return true;
}

} // namespace Debugger
