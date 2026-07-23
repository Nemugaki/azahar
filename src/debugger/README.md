# Render debugger capture format

`.rdbg` is Azahar's current little-endian render-capture contract. The format has no version
number and readers do not contain compatibility branches. It is an immutable render capture, not
an emulator-state rewind.

The 60-byte header contains, in order: the eight-byte `RDBG\r\n\x1a\n` signature; `u32` flags
(complete and truncated); `u64` capability bits; `u32` producer and backend byte lengths; `u32`
gap, timeline, register-write, draw, shader, and resource counts; and `u64` owned payload bytes.
Capabilities are timeline `1`, register writes `2`, resources `4`, shaders `8`, immutable
register state `16`, and outputs `32`.

Producer and backend strings follow without terminators, then:

- Gaps: a 20-byte frame/draw/role/slot/reason-size header followed by UTF-8 reason bytes.
- Timeline: seventeen `u32` values (68 bytes): sequence, kind, frame, global draw, frame-local
  draw, changed mask, target fields, and draw fields.
- Register writes: four `u32` values (16 bytes): bank, index, value, and mask.
- Draws: a 24-byte association header, exactly `0x300` little-endian PICA register words, then
  20-byte resource references containing role, slot, resource ID, and capture phase.
- Shaders: the 28-byte ID/stage/entry/code-count/metadata-count/state-size header followed by
  code, metadata, and complete canonical shader state.
- Resources: a 48-byte ID/role/format/address/width/height/stride/tiling/origin/size header followed
  by owned bytes.

Readers reject unknown flags or capabilities, invalid enums, non-increasing sequences, dangling
references, incorrect owned-byte totals, trailing data, and configured size/count-limit
violations. A referenced empty resource or structured gap makes validation incomplete.

Output references identify draw input, pre-draw, or post-draw phase explicitly. RPC-owned output
capture is independent of debugger widgets; atomic frame capture enables it for the capture, and
the one-shot frame-local draw breakpoint retains it through the pre- and post-draw stops.
