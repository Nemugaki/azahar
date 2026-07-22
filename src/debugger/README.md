# Render debugger capture format

`.rdbg` is a fixed little-endian layout for exchanging immutable render-debugger captures. It can
carry the timeline, Pica register writes, draw associations, vertex shaders, and owned resource
bytes. It is a render capture, not an emulator-state rewind.

The 64-byte file header is:

| Field | Size | Value |
| --- | ---: | --- |
| Magic | 8 | `RDBG\r\n\x1a\n` |
| Major | 2 | `1` |
| Minor | 2 | `0` |
| Flags | 4 | bit 0 complete; bit 1 truncated |
| Capabilities | 8 | bits described below |
| Producer size | 4 | bytes |
| Backend size | 4 | bytes |
| Gap size | 4 | bytes |
| Timeline count | 4 | entries |
| Register-write count | 4 | entries |
| Draw count | 4 | entries |
| Shader count | 4 | entries |
| Resource count | 4 | entries |
| Owned bytes | 8 | shader and resource payload bytes |

Capability bits are timeline `1`, register writes `2`, resources `4`, and shaders `8`. Timeline is
mandatory; a payload section may only be non-empty when its capability is set.

The header is followed by producer, backend, and gap strings with no terminators, then these
sections in order:

- Timeline: 64 bytes per entry; sixteen `u32` values: sequence, kind, frame, draw, changed mask,
  color address, depth address, width, height, color format, depth format, draw mode, vertex count,
  topology, vertex offset, and vertex-shader entry point.
- Register writes: 16 bytes each: bank, index, value, and mask as `u32`.
- Draws: a 24-byte header containing timeline sequence, write begin/count, resource-reference
  count, and shader ID. Each header is immediately followed by that many 16-byte references:
  resource role, slot, and resource ID.
- Shaders: a 28-byte header containing ID, stage, entry point, code-word count, metadata-word
  count, and state-byte count. Little-endian code words, metadata words, and opaque state bytes
  immediately follow.
- Resources: a 40-byte header containing ID, role, format, address, width, height, stride, and byte
  count. Owned bytes immediately follow.

The optional gap string explains missing data and marks the capture incomplete. Readers reject
unknown flags or capabilities, mismatched IDs/ranges/sizes, invalid enum values, non-increasing
sequence numbers, and configured total-byte, owned-byte, or per-category count-limit violations.
