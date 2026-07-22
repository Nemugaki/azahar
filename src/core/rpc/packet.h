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
};

enum class EmulationControl : u32 {
    Status = 0,
    Run = 1,
    Pause = 2,
    Resume = 3,
    Stop = 4,
    Restart = 5,
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

enum class PicaBreakpointOperation : u32 {
    Status = 0,
    Set = 1,
    Resume = 2,
    Clear = 3,
};

enum class PicaTraceOperation : u32 {
    Status = 0,
    Start = 1,
    Stop = 2,
    Read = 3,
    Clear = 4,
};

struct PicaBreakpointReply {
    u32 enabled_mask;
    u32 active_event;
    u32 at_breakpoint;
};

struct PicaTraceReply {
    u32 active;
    u32 generation;
    u32 size;
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
    u32 version;
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

constexpr u32 CURRENT_VERSION = 1;
constexpr u32 MIN_PACKET_SIZE = sizeof(PacketHeader);
constexpr u32 MAX_PACKET_DATA_SIZE = 1024;
constexpr u32 MAX_PACKET_SIZE = MIN_PACKET_SIZE + MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_READ_SIZE = MAX_PACKET_DATA_SIZE;
constexpr u32 MAX_PROCESSES_IN_LIST = (MAX_PACKET_DATA_SIZE - sizeof(u32)) / sizeof(ProcessInfo);
constexpr u32 CAPABILITY_EMULATION_CONTROL = 1U << 0;
constexpr u32 CAPABILITY_PICA_SNAPSHOT = 1U << 1;
constexpr u32 CAPABILITY_PICA_BREAKPOINT = 1U << 2;
constexpr u32 CAPABILITY_PICA_TRACE = 1U << 3;

class Packet {
public:
    explicit Packet(const PacketHeader& header, u8* data,
                    std::function<void(Packet&)> send_reply_callback);
    ~Packet();

    u32 GetVersion() const {
        return header.version;
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
    std::array<u8, MAX_PACKET_DATA_SIZE> packet_data;

    std::function<void(Packet&)> send_reply_callback;
};

} // namespace Core::RPC
