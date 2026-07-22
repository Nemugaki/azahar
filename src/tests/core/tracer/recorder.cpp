// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/tracer/recorder.h"

namespace {

template <typename T>
T Read(std::ifstream& file, std::streamoff offset) {
    T value{};
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    REQUIRE(file.good());
    return value;
}

} // namespace

TEST_CASE("CiTrace recorder writes a self-consistent file", "[core][tracer]") {
    CiTrace::Recorder::InitialState state;
    state.lcd_registers = {0x11111111, 0x22222222};
    state.pica_registers = {0x33333333, 0x44444444, 0x55555555};
    state.default_attributes = {0x66666666};
    state.vs_program_binary = {0x77777777};
    state.vs_swizzle_data = {0x88888888};
    state.vs_float_uniforms = {0x99999999};
    state.gs_program_binary = {0xaaaaaaaa};
    state.gs_swizzle_data = {0xbbbbbbbb};
    state.gs_float_uniforms = {0xcccccccc};

    CiTrace::Recorder recorder{state};
    const std::array<u8, 4> memory{0xde, 0xad, 0xbe, 0xef};
    recorder.MemoryAccessed(memory.data(), memory.size(), 0x12340000);
    recorder.MemoryAccessed(memory.data(), memory.size(), 0x56780000);
    recorder.RegisterWritten(0x1ef00000, 0xabcdef01);
    recorder.FrameFinished();

    const auto path = std::filesystem::temp_directory_path() / "azahar-citrace-test.ctf";
    std::filesystem::remove(path);
    recorder.Finish(path.string());

    std::ifstream file{path, std::ios::binary};
    REQUIRE(file.good());
    const auto header = Read<CiTrace::CTHeader>(file, 0);
    REQUIRE(std::memcmp(header.magic, CiTrace::CTHeader::ExpectedMagicWord(), 4) == 0);
    REQUIRE(header.version == CiTrace::CTHeader::ExpectedVersion());
    REQUIRE(header.header_size == sizeof(header));

    const auto& initial = header.initial_state_offsets;
    REQUIRE(initial.gpu_registers == sizeof(header));
    REQUIRE(initial.gpu_registers_size == 0);
    REQUIRE(initial.lcd_registers == sizeof(header));
    REQUIRE(initial.pica_registers == initial.lcd_registers + state.lcd_registers.size() * 4);
    REQUIRE(initial.default_attributes == initial.pica_registers + state.pica_registers.size() * 4);
    REQUIRE(Read<u32>(file, initial.lcd_registers) == state.lcd_registers.front());
    REQUIRE(Read<u32>(file, initial.pica_registers) == state.pica_registers.front());

    REQUIRE(header.stream_size == 4);
    REQUIRE(header.stream_offset ==
            initial.gs_float_uniforms + state.gs_float_uniforms.size() * 4 + memory.size());
    std::array<u8, 4> stored_memory{};
    file.seekg(header.stream_offset - memory.size());
    file.read(reinterpret_cast<char*>(stored_memory.data()), stored_memory.size());
    REQUIRE(stored_memory == memory);

    const auto first = Read<CiTrace::CTStreamElement>(file, header.stream_offset);
    const auto second = Read<CiTrace::CTStreamElement>(file, header.stream_offset +
                                                                 sizeof(CiTrace::CTStreamElement));
    REQUIRE(first.type == CiTrace::MemoryLoad);
    REQUIRE(second.type == CiTrace::MemoryLoad);
    REQUIRE(first.memory_load.file_offset == second.memory_load.file_offset);
    REQUIRE(first.memory_load.file_offset == header.stream_offset - memory.size());
    REQUIRE(first.memory_load.physical_address == 0x12340000);
    REQUIRE(second.memory_load.physical_address == 0x56780000);

    const auto end = std::filesystem::file_size(path);
    REQUIRE(end == header.stream_offset + header.stream_size * sizeof(CiTrace::CTStreamElement));
    std::filesystem::remove(path);
}
