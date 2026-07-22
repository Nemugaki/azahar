# Render debugger capture format

`.rdbg` is a fixed little-endian layout for exchanging immutable render-debugger captures. It
carries timeline metadata only; it does not contain historical render-target bytes or enough state
to replay a draw.

The 40-byte file header is:

| Field | Size | Value |
| --- | ---: | --- |
| Magic | 8 | `RDBG\r\n\x1a\n` |
| Major | 2 | `1` |
| Minor | 2 | `0` |
| Flags | 4 | bit 0 complete; bit 1 truncated |
| Capabilities | 8 | `1` (timeline) |
| Producer size | 4 | bytes |
| Backend size | 4 | bytes |
| Gap size | 4 | bytes |
| Timeline count | 4 | entries |

The header is followed by the producer, backend, and gap strings with no terminators, then exactly
`timeline count` 64-byte entries. Each entry is sixteen `u32` values in this order: sequence, kind,
frame, draw, changed mask, color address, depth address, width, height, color format, depth format,
draw mode, vertex count, topology, vertex offset, and vertex-shader entry point.

The optional gap string explains missing data and marks the capture incomplete. Readers reject
unknown flags or capabilities, mismatched sizes, invalid enum values, non-increasing sequence
numbers, and configured size/count-limit violations.
