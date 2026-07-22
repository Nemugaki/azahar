// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "debugger/capture_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>

#include "common/swap.h"

namespace Debugger {
namespace {

constexpr std::array<u8, 8> Magic{'R', 'D', 'B', 'G', '\r', '\n', 0x1A, '\n'};
constexpr u16 MajorVersion = 1;
constexpr u16 MinorVersion = 0;
constexpr u32 Complete = 1U << 0;
constexpr u32 Truncated = 1U << 1;
constexpr u64 KnownCapabilities = Timeline | RegisterWrites | Resources | Shaders;
constexpr std::size_t MaxIdentityLength = 1024;
constexpr std::size_t MaxGapLength = 4096;

#pragma pack(push, 1)
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
    u32_le write_count;
    u32_le draw_count;
    u32_le shader_count;
    u32_le resource_count;
    u64_le owned_bytes;
};
static_assert(sizeof(FileHeader) == 64);

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

struct RegisterWriteDisk {
    u32_le bank;
    u32_le index;
    u32_le value;
    u32_le mask;
};
static_assert(sizeof(RegisterWriteDisk) == 16);

struct DrawDisk {
    u32_le timeline_sequence;
    u32_le write_begin;
    u32_le write_count;
    u32_le resource_count;
    u64_le shader_id;
};
static_assert(sizeof(DrawDisk) == 24);

struct ResourceReferenceDisk {
    u32_le role;
    u32_le slot;
    u64_le resource_id;
};
static_assert(sizeof(ResourceReferenceDisk) == 16);

struct ShaderDisk {
    u64_le id;
    u32_le stage;
    u32_le entry_point;
    u32_le code_count;
    u32_le metadata_count;
    u32_le state_size;
};
static_assert(sizeof(ShaderDisk) == 28);

struct ResourceDisk {
    u64_le id;
    u32_le role;
    u32_le format;
    u64_le address;
    u32_le width;
    u32_le height;
    u32_le stride;
    u32_le byte_size;
};
static_assert(sizeof(ResourceDisk) == 40);
#pragma pack(pop)

template <typename T>
bool Read(std::istream& input, T& value) {
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void Write(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool ReadString(std::istream& input, std::string& value, std::size_t size) {
    value.resize(size);
    return !size || static_cast<bool>(input.read(value.data(), size));
}

bool ValidTimelineEntry(const TimelineEntry& entry) {
    return entry.kind <= TimelineKind::Frame && entry.draw_info.mode <= DrawMode::Immediate;
}

bool ValidRole(ResourceRole role) {
    return role <= ResourceRole::Other;
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

bool SameTimeline(const TimelineEntry& left, const TimelineEntry& right) {
    const auto left_disk = ToDisk(left);
    const auto right_disk = ToDisk(right);
    return std::memcmp(&left_disk, &right_disk, sizeof(left_disk)) == 0;
}

void WriteWords(std::ostream& output, std::span<const u32> words) {
    for (u32 word : words) {
        Write(output, u32_le{word});
    }
}

bool ReadWords(std::istream& input, std::vector<u32>& words, u32 count) {
    words.reserve(count);
    for (u32 index = 0; index < count; ++index) {
        u32_le word{};
        if (!Read(input, word)) {
            return false;
        }
        words.push_back(word);
    }
    return true;
}

const TimelineEntry* FindTimeline(const Capture& capture, u32 sequence) {
    const auto found = std::ranges::lower_bound(capture.timeline, sequence, {},
                                                [](const auto& entry) { return entry.sequence; });
    return found != capture.timeline.end() && found->sequence == sequence ? &*found : nullptr;
}

bool ValidateCapture(const Capture& capture, std::string& error) {
    if ((capture.capabilities & Timeline) == 0 ||
        (capture.capabilities & ~KnownCapabilities) != 0 ||
        capture.producer.size() > MaxIdentityLength || capture.backend.size() > MaxIdentityLength ||
        capture.gap_reason.size() > MaxGapLength ||
        capture.timeline.size() > std::numeric_limits<u32>::max() ||
        capture.writes.size() > std::numeric_limits<u32>::max() ||
        capture.draws.size() > std::numeric_limits<u32>::max() ||
        capture.shaders.size() > std::numeric_limits<u32>::max() ||
        capture.resources.size() > std::numeric_limits<u32>::max()) {
        error = "capture metadata is invalid";
        return false;
    }
    if ((!capture.writes.empty() && !(capture.capabilities & RegisterWrites)) ||
        (!capture.shaders.empty() && !(capture.capabilities & Shaders)) ||
        (!capture.resources.empty() && !(capture.capabilities & Resources))) {
        error = "capture capability data is inconsistent";
        return false;
    }
    for (std::size_t index = 0; index < capture.timeline.size(); ++index) {
        if (!ValidTimelineEntry(capture.timeline[index]) ||
            (index && capture.timeline[index - 1].sequence >= capture.timeline[index].sequence)) {
            error = "capture timeline is invalid";
            return false;
        }
    }

    std::unordered_set<u64> shader_ids;
    u64 calculated_owned{};
    for (const auto& shader : capture.shaders) {
        if (!shader.id || !shader_ids.insert(shader.id).second ||
            shader.code.size() > std::numeric_limits<u32>::max() ||
            shader.metadata.size() > std::numeric_limits<u32>::max() ||
            shader.state.size() > std::numeric_limits<u32>::max()) {
            error = "capture shader is invalid";
            return false;
        }
        const u64 bytes =
            static_cast<u64>(shader.code.size() + shader.metadata.size()) * sizeof(u32) +
            shader.state.size();
        if (bytes > std::numeric_limits<u64>::max() - calculated_owned) {
            error = "capture owned byte count overflowed";
            return false;
        }
        calculated_owned += bytes;
    }

    std::unordered_set<u64> resource_ids;
    for (const auto& resource : capture.resources) {
        if (!resource.id || !ValidRole(resource.role) || !resource_ids.insert(resource.id).second ||
            resource.bytes.size() > std::numeric_limits<u32>::max() ||
            resource.bytes.size() > std::numeric_limits<u64>::max() - calculated_owned) {
            error = "capture resource is invalid";
            return false;
        }
        calculated_owned += resource.bytes.size();
    }
    if (calculated_owned != capture.owned_bytes) {
        error = "capture owned byte count is invalid";
        return false;
    }

    for (const auto& draw : capture.draws) {
        const auto* timeline = FindTimeline(capture, draw.timeline.sequence);
        if (!timeline || !SameTimeline(*timeline, draw.timeline) ||
            draw.write_begin > capture.writes.size() ||
            draw.write_count > capture.writes.size() - draw.write_begin ||
            draw.resources.size() > std::numeric_limits<u32>::max() ||
            (draw.shader_id && !shader_ids.contains(draw.shader_id))) {
            error = "capture draw is invalid";
            return false;
        }
        for (const auto& reference : draw.resources) {
            if (!ValidRole(reference.role) || !resource_ids.contains(reference.resource_id)) {
                error = "capture resource reference is invalid";
                return false;
            }
        }
    }
    return true;
}

bool AddOwned(u64 bytes, u64 limit, u64& total) {
    if (bytes > limit - std::min(limit, total)) {
        return false;
    }
    total += bytes;
    return true;
}

} // namespace

bool SaveCapture(const std::string& path, const Capture& capture, std::string& error) {
    error.clear();
    if (!ValidateCapture(capture, error)) {
        return false;
    }

    const u32 flags = (capture.complete && capture.gap_reason.empty() ? Complete : 0) |
                      (capture.truncated ? Truncated : 0);
    const FileHeader header{Magic,
                            MajorVersion,
                            MinorVersion,
                            flags,
                            capture.capabilities,
                            static_cast<u32>(capture.producer.size()),
                            static_cast<u32>(capture.backend.size()),
                            static_cast<u32>(capture.gap_reason.size()),
                            static_cast<u32>(capture.timeline.size()),
                            static_cast<u32>(capture.writes.size()),
                            static_cast<u32>(capture.draws.size()),
                            static_cast<u32>(capture.shaders.size()),
                            static_cast<u32>(capture.resources.size()),
                            capture.owned_bytes};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open capture for writing";
        return false;
    }
    Write(output, header);
    output.write(capture.producer.data(), capture.producer.size());
    output.write(capture.backend.data(), capture.backend.size());
    output.write(capture.gap_reason.data(), capture.gap_reason.size());
    for (const auto& entry : capture.timeline) {
        Write(output, ToDisk(entry));
    }
    for (const auto& write : capture.writes) {
        Write(output, RegisterWriteDisk{write.bank, write.index, write.value, write.mask});
    }
    for (const auto& draw : capture.draws) {
        Write(output, DrawDisk{draw.timeline.sequence, draw.write_begin, draw.write_count,
                               static_cast<u32>(draw.resources.size()), draw.shader_id});
        for (const auto& reference : draw.resources) {
            Write(output, ResourceReferenceDisk{static_cast<u32>(reference.role), reference.slot,
                                                reference.resource_id});
        }
    }
    for (const auto& shader : capture.shaders) {
        Write(output, ShaderDisk{shader.id, shader.stage, shader.entry_point,
                                 static_cast<u32>(shader.code.size()),
                                 static_cast<u32>(shader.metadata.size()),
                                 static_cast<u32>(shader.state.size())});
        WriteWords(output, shader.code);
        WriteWords(output, shader.metadata);
        output.write(reinterpret_cast<const char*>(shader.state.data()), shader.state.size());
    }
    for (const auto& resource : capture.resources) {
        Write(output, ResourceDisk{resource.id, static_cast<u32>(resource.role), resource.format,
                                   resource.address, resource.width, resource.height,
                                   resource.stride, static_cast<u32>(resource.bytes.size())});
        output.write(reinterpret_cast<const char*>(resource.bytes.data()), resource.bytes.size());
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
        (static_cast<u64>(header.capabilities) & Timeline) == 0 ||
        (static_cast<u64>(header.capabilities) & ~KnownCapabilities) != 0) {
        error = "unsupported capture header";
        return false;
    }

    const u32 producer_size = header.producer_size;
    const u32 backend_size = header.backend_size;
    const u32 gap_size = header.gap_size;
    const u32 timeline_count = header.timeline_count;
    const u32 write_count = header.write_count;
    const u32 draw_count = header.draw_count;
    const u32 shader_count = header.shader_count;
    const u32 resource_count = header.resource_count;
    if (producer_size > MaxIdentityLength || backend_size > MaxIdentityLength ||
        gap_size > MaxGapLength || timeline_count > limits.timeline_entries ||
        write_count > limits.register_writes || draw_count > limits.draws ||
        shader_count > limits.shaders || resource_count > limits.resources ||
        static_cast<u64>(header.owned_bytes) > limits.owned_bytes) {
        error = "capture exceeds configured limits";
        return false;
    }
    const u64 minimum_size = sizeof(FileHeader) + static_cast<u64>(producer_size) + backend_size +
                             gap_size +
                             static_cast<u64>(timeline_count) * sizeof(TimelineDiskEntry) +
                             static_cast<u64>(write_count) * sizeof(RegisterWriteDisk) +
                             static_cast<u64>(draw_count) * sizeof(DrawDisk) +
                             static_cast<u64>(shader_count) * sizeof(ShaderDisk) +
                             static_cast<u64>(resource_count) * sizeof(ResourceDisk);
    if (minimum_size > static_cast<u64>(end)) {
        error = "invalid capture size";
        return false;
    }

    Capture loaded;
    loaded.capabilities = header.capabilities;
    loaded.complete = (static_cast<u32>(header.flags) & Complete) != 0 && gap_size == 0;
    loaded.truncated = (static_cast<u32>(header.flags) & Truncated) != 0;
    loaded.owned_bytes = header.owned_bytes;
    if (!ReadString(input, loaded.producer, producer_size) ||
        !ReadString(input, loaded.backend, backend_size) ||
        !ReadString(input, loaded.gap_reason, gap_size)) {
        error = "truncated capture metadata";
        return false;
    }

    loaded.timeline.reserve(timeline_count);
    for (u32 index = 0; index < timeline_count; ++index) {
        TimelineDiskEntry disk{};
        if (!Read(input, disk)) {
            error = "truncated capture timeline";
            return false;
        }
        loaded.timeline.push_back(FromDisk(disk));
    }
    loaded.writes.reserve(write_count);
    for (u32 index = 0; index < write_count; ++index) {
        RegisterWriteDisk disk{};
        if (!Read(input, disk)) {
            error = "truncated capture register writes";
            return false;
        }
        loaded.writes.push_back({disk.bank, disk.index, disk.value, disk.mask});
    }
    loaded.draws.reserve(draw_count);
    for (u32 index = 0; index < draw_count; ++index) {
        DrawDisk disk{};
        if (!Read(input, disk)) {
            error = "truncated capture draws";
            return false;
        }
        const auto* timeline = FindTimeline(loaded, disk.timeline_sequence);
        if (!timeline) {
            error = "capture draw has no timeline entry";
            return false;
        }
        DrawCapture draw{*timeline, disk.write_begin, disk.write_count, disk.shader_id, {}};
        draw.resources.reserve(static_cast<u32>(disk.resource_count));
        for (u32 resource = 0; resource < static_cast<u32>(disk.resource_count); ++resource) {
            ResourceReferenceDisk reference{};
            if (!Read(input, reference)) {
                error = "truncated capture resource references";
                return false;
            }
            draw.resources.push_back({static_cast<ResourceRole>(static_cast<u32>(reference.role)),
                                      reference.slot, reference.resource_id});
        }
        loaded.draws.push_back(std::move(draw));
    }

    u64 read_owned{};
    loaded.shaders.reserve(shader_count);
    for (u32 index = 0; index < shader_count; ++index) {
        ShaderDisk disk{};
        if (!Read(input, disk)) {
            error = "truncated capture shader header";
            return false;
        }
        const u64 bytes = static_cast<u64>(static_cast<u32>(disk.code_count) +
                                           static_cast<u32>(disk.metadata_count)) *
                              sizeof(u32) +
                          static_cast<u32>(disk.state_size);
        if (!AddOwned(bytes, limits.owned_bytes, read_owned)) {
            error = "capture shader bytes exceed configured limits";
            return false;
        }
        Shader shader{disk.id, disk.stage, disk.entry_point, {}, {}, {}};
        shader.state.resize(static_cast<u32>(disk.state_size));
        if (!ReadWords(input, shader.code, disk.code_count) ||
            !ReadWords(input, shader.metadata, disk.metadata_count) ||
            (!shader.state.empty() &&
             !input.read(reinterpret_cast<char*>(shader.state.data()), shader.state.size()))) {
            error = "truncated capture shader";
            return false;
        }
        loaded.shaders.push_back(std::move(shader));
    }
    loaded.resources.reserve(resource_count);
    for (u32 index = 0; index < resource_count; ++index) {
        ResourceDisk disk{};
        if (!Read(input, disk) ||
            !AddOwned(static_cast<u32>(disk.byte_size), limits.owned_bytes, read_owned)) {
            error = "invalid capture resource header";
            return false;
        }
        Resource resource{disk.id,      static_cast<ResourceRole>(static_cast<u32>(disk.role)),
                          disk.address, disk.format,
                          disk.width,   disk.height,
                          disk.stride,  {}};
        resource.bytes.resize(static_cast<u32>(disk.byte_size));
        if (!resource.bytes.empty() &&
            !input.read(reinterpret_cast<char*>(resource.bytes.data()), resource.bytes.size())) {
            error = "truncated capture resource";
            return false;
        }
        loaded.resources.push_back(std::move(resource));
    }
    if (read_owned != loaded.owned_bytes || input.peek() != std::char_traits<char>::eof() ||
        !ValidateCapture(loaded, error)) {
        if (error.empty()) {
            error = "invalid capture size";
        }
        return false;
    }
    capture = std::move(loaded);
    return true;
}

} // namespace Debugger
