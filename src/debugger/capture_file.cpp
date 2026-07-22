// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "debugger/capture_file.h"

#include <array>
#include <fstream>
#include <limits>

#include "common/swap.h"

namespace Debugger {
namespace {

constexpr std::array<u8, 8> Magic{'R', 'D', 'B', 'G', '\r', '\n', 0x1A, '\n'};
constexpr u16 MajorVersion = 1;
constexpr u16 MinorVersion = 0;
constexpr u32 Complete = 1U << 0;
constexpr u32 Truncated = 1U << 1;
constexpr std::size_t MaxIdentityLength = 1024;
constexpr std::size_t MaxGapLength = 4096;

struct FileHeader {
    std::array<u8, 8> magic;
    u16_le major;
    u16_le minor;
    u32_le flags;
    u64_le capabilities;
    u32_le producer_size;
    u32_le backend_size;
    u32_le gap_size;
    u32_le timeline_count;
};
static_assert(sizeof(FileHeader) == 40);

struct TimelineDiskEntry {
    u32_le sequence;
    u32_le kind;
    u32_le frame;
    u32_le draw;
    u32_le changed_mask;
    u32_le color_address;
    u32_le depth_address;
    u32_le width;
    u32_le height;
    u32_le color_format;
    u32_le depth_format;
    u32_le draw_mode;
    u32_le vertex_count;
    u32_le topology;
    u32_le vertex_offset;
    u32_le vertex_shader_entry;
};
static_assert(sizeof(TimelineDiskEntry) == 64);

template <typename T>
bool Read(std::istream& input, T& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool ReadString(std::istream& input, std::string& value, std::size_t size) {
    value.resize(size);
    return !size || static_cast<bool>(input.read(value.data(), size));
}

bool ValidTimelineEntry(const TimelineEntry& entry) {
    return entry.kind <= TimelineKind::Frame && entry.draw_info.mode <= DrawMode::Immediate;
}

TimelineDiskEntry ToDisk(const TimelineEntry& entry) {
    return {entry.sequence,
            static_cast<u32>(entry.kind),
            entry.frame,
            entry.draw,
            entry.changed_mask,
            entry.target.color_address,
            entry.target.depth_address,
            entry.target.width,
            entry.target.height,
            entry.target.color_format,
            entry.target.depth_format,
            static_cast<u32>(entry.draw_info.mode),
            entry.draw_info.vertex_count,
            entry.draw_info.topology,
            entry.draw_info.vertex_offset,
            entry.draw_info.vertex_shader_entry};
}

TimelineEntry FromDisk(const TimelineDiskEntry& entry) {
    return {entry.sequence,
            static_cast<TimelineKind>(static_cast<u32>(entry.kind)),
            entry.frame,
            entry.draw,
            entry.changed_mask,
            {entry.color_address, entry.depth_address, entry.width, entry.height,
             entry.color_format, entry.depth_format},
            {static_cast<DrawMode>(static_cast<u32>(entry.draw_mode)), entry.vertex_count,
             entry.topology, entry.vertex_offset, entry.vertex_shader_entry}};
}

} // namespace

bool SaveCapture(const std::string& path, const Capture& capture, std::string& error) {
    error.clear();
    if (capture.capabilities != CaptureCapability::Timeline ||
        capture.producer.size() > MaxIdentityLength || capture.backend.size() > MaxIdentityLength ||
        capture.gap_reason.size() > MaxGapLength ||
        capture.timeline.size() > std::numeric_limits<u32>::max()) {
        error = "capture metadata is invalid";
        return false;
    }
    for (std::size_t index = 0; index < capture.timeline.size(); ++index) {
        if (!ValidTimelineEntry(capture.timeline[index]) ||
            (index && capture.timeline[index - 1].sequence >= capture.timeline[index].sequence)) {
            error = "capture timeline is invalid";
            return false;
        }
    }

    const u32 flags = (capture.complete && capture.gap_reason.empty() ? Complete : 0) |
                      (capture.truncated ? Truncated : 0);
    const FileHeader header{Magic,
                            MajorVersion,
                            MinorVersion,
                            flags,
                            CaptureCapability::Timeline,
                            static_cast<u32>(capture.producer.size()),
                            static_cast<u32>(capture.backend.size()),
                            static_cast<u32>(capture.gap_reason.size()),
                            static_cast<u32>(capture.timeline.size())};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open capture for writing";
        return false;
    }
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(capture.producer.data(), capture.producer.size());
    output.write(capture.backend.data(), capture.backend.size());
    output.write(capture.gap_reason.data(), capture.gap_reason.size());
    for (const auto& entry : capture.timeline) {
        const auto disk_entry = ToDisk(entry);
        output.write(reinterpret_cast<const char*>(&disk_entry), sizeof(disk_entry));
    }
    if (!output) {
        error = "failed while writing capture";
        return false;
    }
    return true;
}

bool LoadCapture(const std::string& path, Capture& capture, std::string& error,
                 const CaptureLimits& limits) {
    error.clear();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto end = input.tellg();
    if (!input) {
        error = "could not open capture";
        return false;
    }
    if (end < 0 || static_cast<u64>(end) > limits.total_bytes ||
        static_cast<u64>(end) < sizeof(FileHeader)) {
        error = "capture exceeds configured limits";
        return false;
    }
    input.seekg(0);

    FileHeader header{};
    if (!Read(input, header) || header.magic != Magic ||
        static_cast<u16>(header.major) != MajorVersion ||
        static_cast<u16>(header.minor) != MinorVersion ||
        (static_cast<u32>(header.flags) & ~(Complete | Truncated)) != 0 ||
        static_cast<u64>(header.capabilities) != CaptureCapability::Timeline) {
        error = "unsupported capture header";
        return false;
    }

    const u32 producer_size = header.producer_size;
    const u32 backend_size = header.backend_size;
    const u32 gap_size = header.gap_size;
    const u32 timeline_count = header.timeline_count;
    const u64 expected_size = sizeof(FileHeader) + static_cast<u64>(producer_size) + backend_size +
                              gap_size +
                              static_cast<u64>(timeline_count) * sizeof(TimelineDiskEntry);
    if (producer_size > MaxIdentityLength || backend_size > MaxIdentityLength ||
        gap_size > MaxGapLength || timeline_count > limits.timeline_entries ||
        expected_size != static_cast<u64>(end)) {
        error = "invalid capture size";
        return false;
    }

    Capture loaded;
    loaded.capabilities = CaptureCapability::Timeline;
    loaded.complete = (static_cast<u32>(header.flags) & Complete) != 0 && gap_size == 0;
    loaded.truncated = (static_cast<u32>(header.flags) & Truncated) != 0;
    if (!ReadString(input, loaded.producer, producer_size) ||
        !ReadString(input, loaded.backend, backend_size) ||
        !ReadString(input, loaded.gap_reason, gap_size)) {
        error = "truncated capture metadata";
        return false;
    }
    loaded.timeline.reserve(timeline_count);
    for (u32 index = 0; index < timeline_count; ++index) {
        TimelineDiskEntry disk_entry{};
        if (!Read(input, disk_entry)) {
            error = "truncated capture timeline";
            return false;
        }
        auto entry = FromDisk(disk_entry);
        if (!ValidTimelineEntry(entry) ||
            (!loaded.timeline.empty() && loaded.timeline.back().sequence >= entry.sequence)) {
            error = "invalid capture timeline";
            return false;
        }
        loaded.timeline.push_back(entry);
    }
    capture = std::move(loaded);
    return true;
}

} // namespace Debugger
