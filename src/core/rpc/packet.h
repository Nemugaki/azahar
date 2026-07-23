// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include "common/common_types.h"

namespace Core::RPC {

enum class PacketType : u32 {
    Undefined = 0,
    ReadMemory = 1,
    WriteMemory = 2,
    ProcessList = 3,
    SetGetProcess = 4,
    Capabilities = 5,
    EmulationControl = 6,
    PicaSnapshot = 7,
    PicaBreakpoint = 8,
    PicaTrace = 9,
    CPURegisters = 10,
    GXCommandTrace = 11,
    PicaShader = 12,
    PicaTimeline = 13,
    PicaRenderTarget = 14,
    DebugState = 15,
    DebugCapture = 16,
    RenderSession = 17,
    RenderOutput = 18,
    RenderDebug = 19,
};

enum class EmulationControl : u32 {
    Status = 0,
    Run = 1,
    Pause = 2,
    Resume = 3,
    Stop = 4,
    Restart = 5,
    DebugPause = 6,
    DebugResume = 7,
    SaveState = 8,
    LoadState = 9,
    Screenshot = 10,
    FrameAdvance = 11,
};

enum class EmulationState : u32 {
    Stopped = 0,
    Running = 1,
    Paused = 2,
};

enum class PicaSnapshotOperation : u32 {
    Arm = 0,
    Status = 1,
    Read = 2,
    Clear = 3,
};

enum class PicaTimelineOperation : u32 { Read = 0, Clear = 1, Status = 2 };

enum class PicaBreakpointOperation : u32 {
    Status = 0,
    Set = 1,
    Resume = 2,
    Clear = 3,
    SetCondition = 4,
    GetCondition = 5,
    SetOptions = 6,
    GetOptions = 7,
};

enum class PicaTraceOperation : u32 {
    Status = 0,
    Start = 1,
    Stop = 2,
    Read = 3,
    Clear = 4,
    ReadWrites = 5,
};

enum class GXCommandTraceOperation : u32 {
    Status = 0,
    Start = 1,
    Stop = 2,
    Read = 3,
    Clear = 4,
};

enum class PicaShaderOperation : u32 {
    Status = 0,
    Prepare = 1,
    ReadDump = 2,
    ReadCycles = 3,
    Clear = 4,
};

enum class DebugStateOperation : u32 { Status = 0, Wait = 1 };

enum class DebugCaptureOperation : u32 {
    Create = 0,
    Status = 1,
    Read = 2,
    Diff = 3,
    List = 4,
    Delete = 5,
    Pin = 6,
    CacheStatus = 7,
};

enum class RenderSessionOperation : u32 {
    List = 0,
    Select = 1,
    Remove = 2,
    Import = 3,
    Export = 4,
};

enum class RenderOutputOperation : u32 { Status = 0, Read = 1 };

enum class RenderDebugOperation : u32 {
    Capabilities = 0,
    CaptureFrame = 1,
    Timeline = 2,
    State = 3,
    Validate = 4,
    OutputStatus = 5,
    OutputRead = 6,
    Diff = 7,
    Vertex = 8,
    DrawBreak = 9,
};

enum class Error : u32 {
    InvalidPacket = 1,
    PermissionDenied = 2,
    InvalidState = 3,
    InvalidArgument = 4,
    Unsupported = 5,
    NotFound = 6,
    Busy = 7,
    Failed = 8,
};

struct ErrorReply {
    u32 magic;
    Error error;
};

struct CapabilitiesReply {
    u32 protocol_magic;
    u32 capabilities;
    u32 max_packet_data_size;
    u32 build_id_size;
};

struct DebugStateReply {
    u32 reason;
    u32 detail;
    u32 generation;
};

struct DebugCaptureReply {
    u32 id;
    u32 size;
    u32 reason;
    u32 detail;
    u32 flags;
};

struct DebugCaptureCacheReply {
    u32 count;
    u32 used;
    u32 limit;
};

struct DebugCaptureDiffReply {
    u32 offset;
    u32 before;
    u32 after;
};

struct PicaShaderStatus {
    u32 generation;
    u32 dump_size;
    u32 cycle_count;
    u32 vertex_input_valid;
};

struct PicaShaderCycle {
    u32 cycle;
    u32 instruction_offset;
    u32 next_instruction;
    u32 mask;
    std::array<u32, 20> vectors;
    std::array<s32, 2> address_registers;
    u32 condition_bits;
    u32 loop_int;
};
static_assert(sizeof(PicaShaderCycle) == 0x70);

struct PicaBreakpointReply {
    u32 enabled_mask;
    u32 active_event;
    u32 at_breakpoint;
};

struct PicaBreakpointConditionReply {
    u32 field;
    u32 value;
    u32 mask;
};

struct PicaBreakpointOptionsReply {
    u32 one_shot;
    u32 skip_remaining;
    u32 hit_count;
};

struct PicaRenderTargetReply {
    u32 color_address;
    u32 depth_address;
    u32 width;
    u32 height;
    u32 color_format;
    u32 depth_format;
};

struct PicaTimelineEntry {
    u32 sequence;
    u32 kind;
    u32 frame;
    u32 draw;
    u32 frame_draw;
    u32 changed_mask;
    PicaRenderTargetReply target;
    u32 draw_mode;
    u32 vertex_count;
    u32 topology;
    u32 vertex_offset;
    u32 vertex_shader_entry;
};
static_assert(sizeof(PicaTimelineEntry) == 0x44);

struct PicaTimelineStatus {
    u32 count;
    u32 oldest_sequence;
    u32 newest_sequence;
    u32 truncated;
};

struct RenderSessionReply {
    u64 id;
    u64 capabilities;
    u32 flags;
    std::array<char, 20> producer;
    std::array<char, 20> backend;
};
static_assert(sizeof(RenderSessionReply) == 0x40);

struct RenderOutputReply {
    u64 session_id;
    u32 sequence;
    u32 format;
    u64 address;
    u32 width;
    u32 height;
    u32 stride;
    u32 size;
};
static_assert(sizeof(RenderOutputReply) == 0x28);

struct RenderDebugCapabilitiesReply {
    u64 capture_capabilities;
    u32 max_packet_data_size;
    u32 build_id_size;
};

struct RenderCaptureReply {
    u64 session_id;
    u32 timeline_count;
    u32 issue_count;
    u32 complete;
    u32 truncated;
};

// Wire words: epoch low/high, next sequence, entry count, flags.
using RenderTimelinePageReply = std::array<u32, 5>;
static_assert(sizeof(RenderTimelinePageReply) == 0x14);

struct RenderStatePageReply {
    u32 total;
    u32 start;
    u32 count;
};

struct RenderValidationIssueReply {
    u32 kind;
    u32 frame;
    u32 draw;
    u32 role;
    u32 slot;
    std::array<char, 76> message;
};
static_assert(sizeof(RenderValidationIssueReply) == 0x60);

struct RenderOutputInfoReply {
    u64 session_id;
    u32 sequence;
    u32 phase;
    u64 address;
    u32 format;
    u32 width;
    u32 height;
    u32 stride;
    u32 tiling;
    u32 origin;
    u32 size;
    u32 reserved;
};
static_assert(sizeof(RenderOutputInfoReply) == 0x38);

struct RenderDiffReply {
    u32 changed_mask;
    u32 changed_registers;
    u32 changed_resources;
    u32 changed_pixels;
    u32 min_x;
    u32 min_y;
    u32 max_x;
    u32 max_y;
};

struct RenderDrawBreakReply {
    u32 phase;
    u32 frame_draw;
    u32 color_address;
    u32 filter_color;
};

struct RenderVertexReply {
    u32 ordinal;
    u32 resolved_index;
    u32 loaded_mask;
    u32 fixed_mask;
    u32 shader_entry;
    u32 reserved;
    u64 shader_id;
    std::array<u8, 16> input_map;
    std::array<f32, 16 * 4> input;
    std::array<f32, 16 * 4> output;
    std::array<f32, 24> semantic;
};
static_assert(sizeof(RenderVertexReply) == 0x290);

struct PicaTraceReply {
    u32 active;
    u32 generation;
    u32 size;
    u32 truncated;
};

enum class EmulationResult : u32 {
    Success = 0,
    InvalidState = 1,
    InvalidArgument = 2,
    Unsupported = 3,
    Failed = 4,
};

struct EmulationControlReply {
    EmulationResult result;
    EmulationState state;
};

using EmulationControlHandler =
    std::function<EmulationControlReply(EmulationControl, const std::string&)>;

struct PacketHeader {
    u32 magic;
    u32 id;
    PacketType packet_type;
    u32 packet_size;
};

#pragma pack(push, 1)
struct ProcessInfo {
    u32 process_id;
    u64 title_id;
    std::array<u8, 8> process_name;
};
static_assert(sizeof(ProcessInfo) == 0x14, "Incorrect ProcessInfo size");
#pragma pack(pop)

constexpr u32 PROTOCOL_MAGIC = 0x525A4841;    // "AHZR" on little-endian hosts.
constexpr u32 ERROR_REPLY_MAGIC = 0x52504345; // "ECPR" on little-endian hosts.
constexpr u32 MIN_PACKET_SIZE = sizeof(PacketHeader);
constexpr u32 MAX_PACKET_DATA_SIZE = 1024;
constexpr u32 MAX_PACKET_SIZE = MIN_PACKET_SIZE + MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_READ_SIZE = MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_PROCESSES_IN_LIST = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(ProcessInfo);
constexpr u32 CAPABILITY_EMULATION_CONTROL = 1U << 0;
constexpr u32 CAPABILITY_PICA_SNAPSHOT = 1U << 1;
constexpr u32 CAPABILITY_PICA_BREAKPOINT = 1U << 2;
constexpr u32 CAPABILITY_PICA_TRACE = 1U << 3;
constexpr u32 CAPABILITY_CPU_REGISTERS = 1U << 4;
constexpr u32 CAPABILITY_GX_COMMAND_TRACE = 1U << 5;
constexpr u32 CAPABILITY_PICA_SHADER = 1U << 6;
constexpr u32 CAPABILITY_MEMORY_ACCESS = 1U << 7;
constexpr u32 CAPABILITY_SAVE_STATES = 1U << 8;
constexpr u32 CAPABILITY_SCREENSHOTS = 1U << 9;
constexpr u32 CAPABILITY_MEMORY_WRITE = 1U << 10;
constexpr u32 CAPABILITY_PICA_TIMELINE = 1U << 11;
constexpr u32 CAPABILITY_PICA_RENDER_TARGET = 1U << 12;
constexpr u32 CAPABILITY_DEBUG_STATE = 1U << 13;
constexpr u32 CAPABILITY_DEBUG_CAPTURE = 1U << 14;
constexpr u32 CAPABILITY_ERROR_REPLIES = 1U << 15;
constexpr u32 CAPABILITY_REQUEST_DEDUPLICATION = 1U << 16;
constexpr u32 CAPABILITY_RENDER_SESSIONS = 1U << 17;
constexpr u32 CAPABILITY_RENDER_OUTPUTS = 1U << 18;
constexpr u32 CAPABILITY_RENDER_DEBUG = 1U << 19;

class Packet {
public:
    explicit Packet(const PacketHeader& header, u8* data,
                    std::function<void(Packet&)> send_reply_callback);
    ~Packet();

    u32 GetMagic() const {
        return header.magic;
    }

    u32 GetId() const {
        return header.id;
    }

    PacketType GetPacketType() const {
        return header.packet_type;
    }

    u32 GetPacketDataSize() const {
        return header.packet_size;
    }

    const PacketHeader& GetHeader() const {
        return header;
    }

    std::span<u8, MAX_PACKET_DATA_SIZE> GetPacketData() {
        return packet_data;
    }

    void SetPacketDataSize(u32 size) {
        header.packet_size = size;
    }

    void SendReply() {
        send_reply_callback(*this);
    }

private:
    struct PacketHeader header;
    std::array<u8, MAX_PACKET_DATA_SIZE> packet_data{};

    std::function<void(Packet&)> send_reply_callback;
};

} // namespace Core::RPC
