// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "debugger/render_session.h"

#include <algorithm>

namespace Debugger {

void RenderSession::SetRenderTarget(RenderTarget target) {
    std::lock_guard lock{mutex};
    render_target = target;
}

RenderTarget RenderSession::GetRenderTarget() const {
    std::lock_guard lock{mutex};
    return render_target;
}

void RenderSession::RecordDraw(const DrawInfo& info) {
    Record(TimelineKind::Draw, info);
}

void RenderSession::RecordFrame() {
    Record(TimelineKind::Frame, {});
}

void RenderSession::Record(TimelineKind kind, const DrawInfo& info) {
    std::lock_guard lock{mutex};
    if (kind == TimelineKind::Draw) {
        ++draw_index;
    } else {
        ++frame_index;
    }

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
    timeline.push_back({sequence++, kind, frame_index, draw_index, changed, render_target, info});
    if (timeline.size() > MaxEntries) {
        timeline.pop_front();
        truncated = true;
    }
    if (kind == TimelineKind::Frame) {
        TrimFrames();
    }
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
               (query.target_address == Latest || entry.target.color_address == query.target_address ||
                entry.target.depth_address == query.target_address) &&
               (query.shader_entry == Latest ||
                entry.draw_info.vertex_shader_entry == query.shader_entry) &&
               (query.frame == Latest || entry.frame == query.frame);
    };
    if (query.start == Latest) {
        for (auto entry = timeline.rbegin(); entry != timeline.rend() && result.size() < query.count;
             ++entry) {
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
    return {static_cast<u32>(timeline.size()), timeline.empty() ? sequence : timeline.front().sequence,
            timeline.empty() ? sequence : timeline.back().sequence, truncated};
}

TimelinePosition RenderSession::GetPosition() const {
    std::lock_guard lock{mutex};
    return {frame_index, draw_index};
}

void RenderSession::Clear() {
    std::lock_guard lock{mutex};
    timeline.clear();
    previous_target = render_target;
    sequence = 0;
    frame_index = 0;
    draw_index = 0;
    truncated = false;
}

void RenderSession::SetFrameLimit(u32 limit) {
    std::lock_guard lock{mutex};
    frame_limit = std::max(1U, limit);
    TrimFrames();
}

bool RenderSession::Replace(std::vector<TimelineEntry> entries, bool was_truncated) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].kind > TimelineKind::Frame ||
            entries[index].draw_info.mode > DrawMode::Immediate ||
            (index && entries[index - 1].sequence >= entries[index].sequence)) {
            return false;
        }
    }

    std::lock_guard lock{mutex};
    const auto first = entries.size() > MaxEntries ? entries.end() - MaxEntries : entries.begin();
    timeline.assign(first, entries.end());
    truncated = was_truncated || first != entries.begin();
    if (timeline.empty()) {
        render_target = {};
        previous_target = {};
        sequence = 0;
        frame_index = 0;
        draw_index = 0;
        return true;
    }
    const auto& last = timeline.back();
    render_target = last.target;
    previous_target = last.target;
    sequence = last.sequence == Latest ? Latest : last.sequence + 1;
    frame_index = last.frame;
    draw_index = last.draw;
    return true;
}

void RenderSession::TrimFrames() {
    while (!timeline.empty() && frame_index >= timeline.front().frame &&
           frame_index - timeline.front().frame >= frame_limit) {
        timeline.pop_front();
        truncated = true;
    }
}

RenderSessionManager::RenderSessionManager() {
    sessions.push_back({{LiveSessionId, "Azahar", "Pica", Timeline, true, true, false},
                        std::make_shared<RenderSession>(), {}});
}

std::shared_ptr<RenderSession> RenderSessionManager::GetLive() const {
    std::lock_guard lock{sessions_mutex};
    return sessions.front().session;
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
        descriptor.truncated |= stored.session->GetStatus().truncated;
        result.push_back(std::move(descriptor));
    }
    return result;
}

u64 RenderSessionManager::AddImported(Capture capture) {
    auto session = std::make_shared<RenderSession>();
    if (!session->Replace(std::move(capture.timeline), capture.truncated)) {
        return 0;
    }
    const bool truncated = session->GetStatus().truncated;
    std::lock_guard lock{sessions_mutex};
    if (sessions.size() >= MaxImportedSessions + 1) {
        return 0;
    }
    SessionDescriptor descriptor{next_id++,
                                 std::move(capture.producer),
                                 std::move(capture.backend),
                                 capture.capabilities,
                                 false,
                                 capture.complete,
                                 truncated};
    sessions.push_back(
        {std::move(descriptor), std::move(session), std::move(capture.gap_reason)});
    active_id = sessions.back().descriptor.id;
    return active_id;
}

bool RenderSessionManager::Snapshot(u64 id, Capture& capture) const {
    std::shared_ptr<RenderSession> session;
    SessionDescriptor descriptor;
    std::string gap_reason;
    {
        std::lock_guard lock{sessions_mutex};
        const auto found = std::ranges::find_if(
            sessions, [id](const auto& stored) { return stored.descriptor.id == id; });
        if (found == sessions.end()) {
            return false;
        }
        session = found->session;
        descriptor = found->descriptor;
        gap_reason = found->gap_reason;
    }
    const auto status = session->GetStatus();
    capture = {descriptor.producer,
               descriptor.backend,
               descriptor.capabilities,
               descriptor.complete,
               descriptor.truncated || status.truncated,
               std::move(gap_reason),
               session->Query({.start = RenderSession::Latest,
                               .count = RenderSession::MaxEntries})};
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
