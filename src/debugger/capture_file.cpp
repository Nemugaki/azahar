// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "debugger/capture_file.h"

#include <array>
#include <fstream>
#include <limits>
#include <string_view>

namespace Debugger {
namespace {

constexpr std::array<std::uint8_t, 8> Magic{'R', 'D', 'B', 'G', '\r', '\n', 0x1A, '\n'};
constexpr std::uint16_t MajorVersion = 1;
constexpr std::uint16_t MinorVersion = 0;
constexpr std::uint32_t Complete = 1U << 0;
constexpr std::uint32_t Truncated = 1U << 1;
constexpr std::size_t MaxIdentityLength = 1024;
constexpr std::size_t MaxGapLength = 4096;

enum class RecordType : std::uint16_t {
    Session = 1,
    Timeline = 2,
    Gap = 3,
    End = 0xFFFF,
};

void WriteU16(std::ostream& output, std::uint16_t value) {
    const std::array bytes{static_cast<std::uint8_t>(value),
                           static_cast<std::uint8_t>(value >> 8)};
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void WriteU32(std::ostream& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.put(static_cast<char>(value >> shift));
    }
}

void WriteU64(std::ostream& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.put(static_cast<char>(value >> shift));
    }
}

bool ReadBytes(std::istream& input, void* data, std::size_t size) {
    return static_cast<bool>(input.read(static_cast<char*>(data), size));
}

bool ReadU16(std::istream& input, std::uint16_t& value) {
    std::array<std::uint8_t, 2> bytes{};
    if (!ReadBytes(input, bytes.data(), bytes.size())) {
        return false;
    }
    value = static_cast<std::uint16_t>(bytes[0]) | static_cast<std::uint16_t>(bytes[1]) << 8;
    return true;
}

bool ReadU32(std::istream& input, std::uint32_t& value) {
    std::array<std::uint8_t, 4> bytes{};
    if (!ReadBytes(input, bytes.data(), bytes.size())) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return true;
}

bool ReadU64(std::istream& input, std::uint64_t& value) {
    std::array<std::uint8_t, 8> bytes{};
    if (!ReadBytes(input, bytes.data(), bytes.size())) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return true;
}

void WriteRecordHeader(std::ostream& output, RecordType type, std::uint32_t flags,
                       std::uint64_t size) {
    WriteU16(output, static_cast<std::uint16_t>(type));
    WriteU16(output, 1);
    WriteU32(output, flags);
    WriteU64(output, size);
}

bool ValidTimelineEntry(const TimelineEntry& entry) {
    return entry.kind <= TimelineKind::Frame && entry.draw_info.mode <= DrawMode::Immediate;
}

void WriteTimelineEntry(std::ostream& output, const TimelineEntry& entry) {
    WriteU32(output, entry.sequence);
    WriteU32(output, static_cast<u32>(entry.kind));
    WriteU32(output, entry.frame);
    WriteU32(output, entry.draw);
    WriteU32(output, entry.changed_mask);
    WriteU32(output, entry.target.color_address);
    WriteU32(output, entry.target.depth_address);
    WriteU32(output, entry.target.width);
    WriteU32(output, entry.target.height);
    WriteU32(output, entry.target.color_format);
    WriteU32(output, entry.target.depth_format);
    WriteU32(output, static_cast<u32>(entry.draw_info.mode));
    WriteU32(output, entry.draw_info.vertex_count);
    WriteU32(output, entry.draw_info.topology);
    WriteU32(output, entry.draw_info.vertex_offset);
    WriteU32(output, entry.draw_info.vertex_shader_entry);
}

bool ReadTimelineEntry(std::istream& input, TimelineEntry& entry) {
    std::uint32_t kind{};
    std::uint32_t mode{};
    if (!ReadU32(input, entry.sequence) || !ReadU32(input, kind) ||
        !ReadU32(input, entry.frame) || !ReadU32(input, entry.draw) ||
        !ReadU32(input, entry.changed_mask) || !ReadU32(input, entry.target.color_address) ||
        !ReadU32(input, entry.target.depth_address) || !ReadU32(input, entry.target.width) ||
        !ReadU32(input, entry.target.height) || !ReadU32(input, entry.target.color_format) ||
        !ReadU32(input, entry.target.depth_format) || !ReadU32(input, mode) ||
        !ReadU32(input, entry.draw_info.vertex_count) ||
        !ReadU32(input, entry.draw_info.topology) ||
        !ReadU32(input, entry.draw_info.vertex_offset) ||
        !ReadU32(input, entry.draw_info.vertex_shader_entry)) {
        return false;
    }
    entry.kind = static_cast<TimelineKind>(kind);
    entry.draw_info.mode = static_cast<DrawMode>(mode);
    return ValidTimelineEntry(entry);
}

bool ReadString(std::istream& input, std::string& value, std::size_t size, std::size_t limit) {
    if (size > limit) {
        return false;
    }
    value.resize(size);
    return !size || ReadBytes(input, value.data(), size);
}

} // namespace

bool SaveCapture(const std::string& path, const Capture& capture, std::string& error) {
    error.clear();
    if (capture.producer.size() > MaxIdentityLength || capture.backend.size() > MaxIdentityLength ||
        capture.gap_reason.size() > MaxGapLength) {
        error = "capture metadata is too large";
        return false;
    }
    for (std::size_t index = 0; index < capture.timeline.size(); ++index) {
        if (!ValidTimelineEntry(capture.timeline[index]) ||
            (index && capture.timeline[index - 1].sequence >= capture.timeline[index].sequence)) {
            error = "capture timeline is invalid";
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not open capture for writing";
        return false;
    }
    output.write(reinterpret_cast<const char*>(Magic.data()), Magic.size());
    WriteU16(output, MajorVersion);
    WriteU16(output, MinorVersion);
    WriteU32(output, 0);

    const std::uint32_t flags = (capture.complete ? Complete : 0) |
                                (capture.truncated ? Truncated : 0);
    WriteRecordHeader(output, RecordType::Session, flags,
                      16 + capture.producer.size() + capture.backend.size());
    WriteU64(output, capture.capabilities);
    WriteU32(output, static_cast<std::uint32_t>(capture.producer.size()));
    WriteU32(output, static_cast<std::uint32_t>(capture.backend.size()));
    output.write(capture.producer.data(), capture.producer.size());
    output.write(capture.backend.data(), capture.backend.size());

    if (!capture.gap_reason.empty()) {
        WriteRecordHeader(output, RecordType::Gap, 0, capture.gap_reason.size());
        output.write(capture.gap_reason.data(), capture.gap_reason.size());
    }
    for (const auto& entry : capture.timeline) {
        WriteRecordHeader(output, RecordType::Timeline, 0, 64);
        WriteTimelineEntry(output, entry);
    }
    WriteRecordHeader(output, RecordType::End, 0, 0);
    if (!output) {
        error = "failed while writing capture";
        return false;
    }
    return true;
}

bool LoadCapture(const std::string& path, Capture& capture, std::string& error,
                 const CaptureLimits& limits) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open capture";
        return false;
    }
    std::array<std::uint8_t, Magic.size()> magic{};
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint32_t reserved{};
    if (!ReadBytes(input, magic.data(), magic.size()) || !ReadU16(input, major) ||
        !ReadU16(input, minor) || !ReadU32(input, reserved) || magic != Magic ||
        major != MajorVersion) {
        error = "unsupported capture header";
        return false;
    }

    Capture loaded;
    bool have_session = false;
    bool have_end = false;
    std::uint64_t consumed = 16;
    while (!have_end) {
        std::uint16_t raw_type{};
        std::uint16_t version{};
        std::uint32_t flags{};
        std::uint64_t size{};
        if (!ReadU16(input, raw_type) || !ReadU16(input, version) || !ReadU32(input, flags) ||
            !ReadU64(input, size)) {
            error = "truncated capture record header";
            return false;
        }
        if (size > limits.record_bytes || consumed > limits.total_bytes ||
            limits.total_bytes - consumed < 16 ||
            size > limits.total_bytes - consumed - 16) {
            error = "capture exceeds configured limits";
            return false;
        }
        consumed += 16 + size;
        const auto type = static_cast<RecordType>(raw_type);
        if (type == RecordType::End) {
            if (size != 0) {
                error = "invalid end record";
                return false;
            }
            have_end = true;
        } else if (type == RecordType::Session && version == 1) {
            std::uint32_t producer_size{};
            std::uint32_t backend_size{};
            if (have_session || size < 16 || !ReadU64(input, loaded.capabilities) ||
                !ReadU32(input, producer_size) || !ReadU32(input, backend_size) ||
                size != 16ULL + producer_size + backend_size ||
                !ReadString(input, loaded.producer, producer_size, MaxIdentityLength) ||
                !ReadString(input, loaded.backend, backend_size, MaxIdentityLength)) {
                error = "invalid session record";
                return false;
            }
            loaded.complete = flags & Complete;
            loaded.truncated = flags & Truncated;
            have_session = true;
        } else if (type == RecordType::Timeline && version == 1) {
            TimelineEntry entry;
            if (!have_session || size != 64 || loaded.timeline.size() >= limits.timeline_entries ||
                !ReadTimelineEntry(input, entry) ||
                (!loaded.timeline.empty() && loaded.timeline.back().sequence >= entry.sequence)) {
                error = "invalid timeline record";
                return false;
            }
            loaded.timeline.push_back(entry);
        } else if (type == RecordType::Gap && version == 1) {
            if (!ReadString(input, loaded.gap_reason, size, MaxGapLength)) {
                error = "invalid gap record";
                return false;
            }
            loaded.complete = false;
        } else {
            input.ignore(static_cast<std::streamsize>(size));
            if (!input) {
                error = "truncated capture record";
                return false;
            }
        }
    }
    if (!have_session) {
        error = "capture has no session record";
        return false;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        error = "capture contains data after the end record";
        return false;
    }
    capture = std::move(loaded);
    return true;
}

} // namespace Debugger
