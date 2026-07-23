// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include "core/memory.h"
#include "video_core/pica/output_vertex.h"
#include "video_core/pica/regs_pipeline.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {

class VertexLoader {
public:
    using ReadCallback = std::function<const u8*(PAddr, std::size_t)>;

    explicit VertexLoader(Memory::MemorySystem& memory_, const PipelineRegs& regs);
    explicit VertexLoader(ReadCallback read_, const PipelineRegs& regs);
    ~VertexLoader();

    bool LoadVertex(PAddr base_address, u32 index, u32 vertex, AttributeBuffer& input,
                    AttributeBuffer& input_default_attributes) const;

    template <typename T>
    bool LoadAttribute(PAddr source_addr, u32 attrib, AttributeBuffer& out) const {
        const T* data = reinterpret_cast<const T*>(
            read(source_addr, vertex_attribute_elements[attrib] * sizeof(T)));
        if (!data) {
            return false;
        }
        for (u32 comp = 0; comp < vertex_attribute_elements[attrib]; ++comp) {
            out[attrib][comp] = f24::FromFloat32(data[comp]);
        }
        return true;
    }

    int GetNumTotalAttributes() const {
        return num_total_attributes;
    }

private:
    ReadCallback read;
    std::array<u32, 16> vertex_attribute_sources;
    std::array<u32, 16> vertex_attribute_strides{};
    std::array<PipelineRegs::VertexAttributeFormat, 16> vertex_attribute_formats;
    std::array<u32, 16> vertex_attribute_elements{};
    std::array<bool, 16> vertex_attribute_is_default;
    int num_total_attributes = 0;
};

} // namespace Pica
