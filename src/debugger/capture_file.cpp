// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "render_debugger/capture_file.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <streambuf>
#include <unordered_set>

namespace Debugger {
namespace {

constexpr std::array<u8, 8> Magic{'R', 'D', 'B', 'G', '\r', '\n', 0x1A, '\n'};
constexpr u32 Complete = 1U << 0;
constexpr u32 Truncated = 1U << 1;
constexpr u64 KnownCapabilities =
    Timeline | RegisterWrites | Resources | Shaders | RegisterState | Outputs;
constexpr std::size_t MaxIdentityLength = 1024;
constexpr std::size_t MaxGapLength = 4096;
constexpr std::size_t MaxGaps = 1U << 20;

template <typename T>
struct LittleEndian {
    std::array<u8, sizeof(T)> bytes{};

    LittleEndian() = default;
    LittleEndian(T value) {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<u8>(value >> (index * 8));
        }
    }
    operator T() const {
        T value{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            value |= static_cast<T>(bytes[index]) << (index * 8);
        }
        return value;
    }
};
using u32_le = LittleEndian<u32>;
using u64_le = LittleEndian<u64>;
static_assert(sizeof(u32_le) == 4);
static_assert(sizeof(u64_le) == 8);

class SpanBuffer final : public std::streambuf {
public:
    explicit SpanBuffer(std::span<const u8> bytes) {
        auto* begin = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        setg(begin, begin, begin + bytes.size());
    }
};

#pragma pack(push, 1)
struct FileHeader {
    std::array<u8, 8> magic;
    u32_le flags;
    u64_le capabilities;
    u32_le producer_size;
    u32_le backend_size;
    u32_le gap_count;
    u32_le timeline_count;
    u32_le write_count;
    u32_le draw_count;
    u32_le shader_count;
    u32_le resource_count;
    u64_le owned_bytes;
};
static_assert(sizeof(FileHeader) == 60);

struct TimelineDiskEntry {
    u32_le sequence;
    u32_le kind;
    u32_le frame;
    u32_le draw;
    u32_le frame_draw;
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
static_assert(sizeof(TimelineDiskEntry) == 68);

struct GapDisk {
    u32_le frame;
    u32_le draw;
    u32_le role;
    u32_le slot;
    u32_le reason_size;
};
static_assert(sizeof(GapDisk) == 20);

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
    u32_le phase;
};
static_assert(sizeof(ResourceReferenceDisk) == 20);

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
    u32_le tiling;
    u32_le origin;
    u32_le byte_size;
};
static_assert(sizeof(ResourceDisk) == 48);
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

bool ValidPhase(CapturePhase phase) {
    return phase <= CapturePhase::PostDraw;
}

TimelineDiskEntry ToDisk(const TimelineEntry& entry) {
    return {entry.sequence,
            static_cast<u32>(entry.kind),
            entry.frame,
            entry.draw,
            entry.frame_draw,
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
            entry.frame_draw,
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
        capture.gaps.size() > MaxGaps ||
        capture.timeline.size() > std::numeric_limits<u32>::max() ||
        capture.writes.size() > std::numeric_limits<u32>::max() ||
        capture.draws.size() > std::numeric_limits<u32>::max() ||
        capture.shaders.size() > std::numeric_limits<u32>::max() ||
        capture.resources.size() > std::numeric_limits<u32>::max()) {
        error = "capture metadata is invalid";
        return false;
    }
    for (const auto& gap : capture.gaps) {
        if (!ValidRole(gap.role) || gap.reason.empty() || gap.reason.size() > MaxGapLength) {
            error = "capture gap is invalid";
            return false;
        }
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
        if (!resource.id || !ValidRole(resource.role) ||
            resource.tiling > ResourceTiling::PicaTiled ||
            resource.origin > ResourceOrigin::BottomLeft ||
            !resource_ids.insert(resource.id).second ||
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
            if (!ValidRole(reference.role) || !ValidPhase(reference.phase) ||
                !resource_ids.contains(reference.resource_id)) {
                error = "capture resource reference is invalid";
                return false;
            }
        }
        if ((capture.capabilities & RegisterState) == 0) {
            error = "capture draw has no immutable register state";
            return false;
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

bool SerializeCapture(const Capture& capture, std::vector<u8>& bytes, std::string& error) {
    error.clear();
    if (!ValidateCapture(capture, error)) {
        return false;
    }

    const u32 flags = (capture.complete && capture.gaps.empty() ? Complete : 0) |
                      (capture.truncated ? Truncated : 0);
    const FileHeader header{Magic,
                            flags,
                            capture.capabilities,
                            static_cast<u32>(capture.producer.size()),
                            static_cast<u32>(capture.backend.size()),
                            static_cast<u32>(capture.gaps.size()),
                            static_cast<u32>(capture.timeline.size()),
                            static_cast<u32>(capture.writes.size()),
                            static_cast<u32>(capture.draws.size()),
                            static_cast<u32>(capture.shaders.size()),
                            static_cast<u32>(capture.resources.size()),
                            capture.owned_bytes};
    std::ostringstream output(std::ios::binary);
    Write(output, header);
    output.write(capture.producer.data(), capture.producer.size());
    output.write(capture.backend.data(), capture.backend.size());
    for (const auto& gap : capture.gaps) {
        Write(output, GapDisk{gap.frame, gap.draw, static_cast<u32>(gap.role), gap.slot,
                              static_cast<u32>(gap.reason.size())});
        output.write(gap.reason.data(), gap.reason.size());
    }
    for (const auto& entry : capture.timeline) {
        Write(output, ToDisk(entry));
    }
    for (const auto& write : capture.writes) {
        Write(output, RegisterWriteDisk{write.bank, write.index, write.value, write.mask});
    }
    for (const auto& draw : capture.draws) {
        Write(output, DrawDisk{draw.timeline.sequence, draw.write_begin, draw.write_count,
                               static_cast<u32>(draw.resources.size()), draw.shader_id});
        WriteWords(output, draw.registers);
        for (const auto& reference : draw.resources) {
            Write(output,
                  ResourceReferenceDisk{static_cast<u32>(reference.role), reference.slot,
                                        reference.resource_id, static_cast<u32>(reference.phase)});
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
        Write(output,
              ResourceDisk{resource.id, static_cast<u32>(resource.role), resource.format,
                           resource.address, resource.width, resource.height, resource.stride,
                           static_cast<u32>(resource.tiling), static_cast<u32>(resource.origin),
                           static_cast<u32>(resource.bytes.size())});
        output.write(reinterpret_cast<const char*>(resource.bytes.data()), resource.bytes.size());
    }
    if (!output) {
        error = "failed while serializing capture";
        return false;
    }
    const std::string serialized = std::move(output).str();
    bytes.assign(serialized.begin(), serialized.end());
    return true;
}

bool SaveCapture(const std::string& path, const Capture& capture, std::string& error) {
    std::vector<u8> bytes;
    if (!SerializeCapture(capture, bytes, error)) {
        return false;
    }

    const std::filesystem::path destination{path};
    const auto temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output ||
        (!bytes.empty() &&
         !output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
        error = "failed while writing capture";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    output.close();
    if (!output) {
        error = "failed while writing capture";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error) {
        error = "could not replace capture: " + rename_error.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool DeserializeCapture(std::span<const u8> bytes, Capture& capture, std::string& error,
                        const CaptureReadLimits& limits) {
    error.clear();
    if (bytes.size() > limits.total_bytes || bytes.size() < sizeof(FileHeader)) {
        error = "capture exceeds configured limits";
        return false;
    }
    SpanBuffer buffer{bytes};
    std::istream input{&buffer};
    const u64 end = bytes.size();

    FileHeader header{};
    if (!Read(input, header) || header.magic != Magic ||
        (static_cast<u32>(header.flags) & ~(Complete | Truncated)) != 0 ||
        (static_cast<u64>(header.capabilities) & Timeline) == 0 ||
        (static_cast<u64>(header.capabilities) & ~KnownCapabilities) != 0) {
        error = "unsupported capture header";
        return false;
    }

    const u32 producer_size = header.producer_size;
    const u32 backend_size = header.backend_size;
    const u32 gap_count = header.gap_count;
    const u32 timeline_count = header.timeline_count;
    const u32 write_count = header.write_count;
    const u32 draw_count = header.draw_count;
    const u32 shader_count = header.shader_count;
    const u32 resource_count = header.resource_count;
    if (producer_size > MaxIdentityLength || backend_size > MaxIdentityLength ||
        gap_count > MaxGaps || timeline_count > limits.capture.timeline_entries ||
        write_count > limits.capture.register_writes || draw_count > limits.capture.draws ||
        shader_count > limits.capture.shaders || resource_count > limits.capture.resources ||
        static_cast<u64>(header.owned_bytes) > limits.capture.owned_bytes) {
        error = "capture exceeds configured limits";
        return false;
    }
    const u64 minimum_size =
        sizeof(FileHeader) + static_cast<u64>(producer_size) + backend_size +
        static_cast<u64>(gap_count) * sizeof(GapDisk) +
        static_cast<u64>(timeline_count) * sizeof(TimelineDiskEntry) +
        static_cast<u64>(write_count) * sizeof(RegisterWriteDisk) +
        static_cast<u64>(draw_count) * (sizeof(DrawDisk) + 0x300 * sizeof(u32)) +
        static_cast<u64>(shader_count) * sizeof(ShaderDisk) +
        static_cast<u64>(resource_count) * sizeof(ResourceDisk);
    if (minimum_size > static_cast<u64>(end)) {
        error = "invalid capture size";
        return false;
    }

    Capture loaded;
    loaded.capabilities = header.capabilities;
    loaded.complete = (static_cast<u32>(header.flags) & Complete) != 0 && gap_count == 0;
    loaded.truncated = (static_cast<u32>(header.flags) & Truncated) != 0;
    loaded.owned_bytes = header.owned_bytes;
    if (!ReadString(input, loaded.producer, producer_size) ||
        !ReadString(input, loaded.backend, backend_size)) {
        error = "truncated capture metadata";
        return false;
    }
    loaded.gaps.reserve(gap_count);
    for (u32 index = 0; index < gap_count; ++index) {
        GapDisk disk{};
        CaptureGap gap;
        if (!Read(input, disk) || static_cast<u32>(disk.reason_size) > MaxGapLength ||
            !ReadString(input, gap.reason, disk.reason_size)) {
            error = "truncated capture gaps";
            return false;
        }
        gap.frame = disk.frame;
        gap.draw = disk.draw;
        gap.role = static_cast<ResourceRole>(static_cast<u32>(disk.role));
        gap.slot = disk.slot;
        loaded.gaps.push_back(std::move(gap));
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
        DrawCapture draw{*timeline, disk.write_begin, disk.write_count, disk.shader_id, {}, {}};
        std::vector<u32> registers;
        if (!ReadWords(input, registers, draw.registers.size())) {
            error = "truncated capture draw registers";
            return false;
        }
        std::ranges::copy(registers, draw.registers.begin());
        draw.resources.reserve(static_cast<u32>(disk.resource_count));
        for (u32 resource = 0; resource < static_cast<u32>(disk.resource_count); ++resource) {
            ResourceReferenceDisk reference{};
            if (!Read(input, reference)) {
                error = "truncated capture resource references";
                return false;
            }
            draw.resources.push_back(
                {static_cast<ResourceRole>(static_cast<u32>(reference.role)), reference.slot,
                 reference.resource_id,
                 static_cast<CapturePhase>(static_cast<u32>(reference.phase))});
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
        if (!AddOwned(bytes, limits.capture.owned_bytes, read_owned)) {
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
            !AddOwned(static_cast<u32>(disk.byte_size), limits.capture.owned_bytes, read_owned)) {
            error = "invalid capture resource header";
            return false;
        }
        Resource resource{disk.id,
                          static_cast<ResourceRole>(static_cast<u32>(disk.role)),
                          disk.address,
                          disk.format,
                          disk.width,
                          disk.height,
                          disk.stride,
                          static_cast<ResourceTiling>(static_cast<u32>(disk.tiling)),
                          static_cast<ResourceOrigin>(static_cast<u32>(disk.origin)),
                          {}};
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

bool LoadCapture(const std::string& path, Capture& capture, std::string& error,
                 const CaptureReadLimits& limits) {
    error.clear();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto end = input.tellg();
    if (!input) {
        error = "could not open capture";
        return false;
    }
    if (end < 0 || static_cast<u64>(end) > limits.total_bytes) {
        error = "capture exceeds configured limits";
        return false;
    }
    std::vector<u8> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "failed while reading capture";
        return false;
    }
    return DeserializeCapture(bytes, capture, error, limits);
}

} // namespace Debugger
