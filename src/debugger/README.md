# Render debugger capture format

`.rdbg` is a little-endian, length-delimited stream for exchanging immutable render-debugger
captures. Version 1 carries timeline metadata only; it does not contain historical render-target
bytes or enough state to replay a draw.

The 16-byte file header is:

| Field | Size | Value |
| --- | ---: | --- |
| Magic | 8 | `RDBG\r\n\x1a\n` |
| Major | 2 | `1` |
| Minor | 2 | `0` |
| Reserved | 4 | `0` |

Each following record starts with `type:u16`, `version:u16`, `flags:u32`, and
`payload_size:u64`. Readers reject unsupported major versions and invalid sizes, but skip unknown
length-delimited record types and versions. A stream ends with type `0xffff` and a zero-length
payload.

Version 1 records:

- Session (`type=1`): `capabilities:u64`, `producer_size:u32`, `backend_size:u32`, followed by the
  UTF-8 producer and backend bytes. Flag bit 0 means complete; bit 1 means truncated.
- Timeline (`type=2`, 64 bytes): sixteen `u32` values in this order: sequence, kind, frame, draw,
  changed mask, color address, depth address, width, height, color format, depth format, draw mode,
  vertex count, topology, vertex offset, vertex-shader entry point.
- Gap (`type=3`): a bounded UTF-8 explanation. Its presence marks the capture incomplete.

Capability bit 0 is timeline metadata. Bits 1–4 reserve register writes, owned resources, shaders,
and previews. Producers must not set a capability until they emit all records needed for it.

Capture-local resource and shader IDs will be 64-bit. Later payloads may use 64-bit addresses even
though the version 1 Pica timeline remains 32-bit for compatibility. Unknown future records remain
safe to skip because every record is independently sized.
