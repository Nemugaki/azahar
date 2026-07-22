# Scripting RPC

When scripting support is compiled in and RPC is enabled, Azahar listens on UDP port 45987 on
`127.0.0.1`. The server starts with the Qt frontend and stays available while games are started,
stopped, or restarted. It is deliberately loopback-only because the protocol can write emulated
memory and has no authentication. Set `AZAHAR_RPC_PORT` before launching Azahar to override the
default port, which is useful for isolated test instances; pass the same port to the client.

`citra.py` provides the Python client. In addition to memory and process access, it supports:

```python
from citra import Citra

c = Citra(timeout=5)
print(c.capabilities())
print(c.status())
c.run("/absolute/path/to/game.3ds")
c.pause()
c.resume()
c.restart()
c.debug_pause()
print(c.cpu_registers(0, 0, 16))
c.debug_resume()
snapshot = c.capture_pica_snapshot()
c.set_pica_breakpoint(2)  # incoming primitive batch
print(c.pica_breakpoints())
c.resume_pica_breakpoint()
c.start_pica_trace()
trace = c.finish_pica_trace()
c.gx_command_trace(1)  # start bounded GSP command history
print(c.gx_command_trace(3, start=0, count=20))
c.set_pica_breakpoint(2)
shader = c.pica_shader_status(1)  # shared Qt/RPC shader snapshot
print(c.pica_shader_cycles(shader[0], count=8))
c.stop()
```

Lifecycle calls return `(result, state)`. PICA capture arms a one-shot snapshot at the beginning of
the next draw. It therefore works with hardware shaders enabled and does not halt emulation.

## PICA snapshot format

Snapshots are little-endian. The 80-byte header contains four `u32` values followed by eight
`(offset, size)` pairs:

1. Magic `PICA` (`0x41434950`), format version, generation, section count.
2. PICA registers.
3. LCD registers.
4. Default vertex attributes as float24 words.
5. Active vertex-shader program words.
6. Active vertex-shader swizzle words.
7. Vertex-shader float uniforms as float24 words.
8. Vertex-shader bool uniform bit mask.
9. Vertex-shader integer uniforms.

The generation lets clients detect a replacement while fetching a snapshot in 1 KiB UDP chunks.

## PICA breakpoints and command traces

Breakpoint events are numbered in the same order as the PICA Breakpoints dock: command loaded,
command processed, incoming primitive batch, finished primitive batch, vertex-shader invocation,
display transfer, GSP command processed, and buffer swapped. The status tuple is
`(enabled_mask, active_event, halted)`. Enabling the vertex-shader invocation breakpoint routes
draws through the software vertex path so it also works when hardware shaders are configured.

PICA command traces contain packed little-endian `<HHI` records: command register ID, write mask,
and value. Trace capture is global, so the Qt command-list tracer and an RPC client should not be
used simultaneously.

## Shared debugger data

The ARM register dock and RPC read the same stopped-core register snapshot. The Graphics Debugger
and RPC share one GSP command history, while the Pica Vertex Shader dock and RPC share one shader
snapshot builder for code, swizzles, input mapping, dump bytes, and interpreter cycles. RPC reads
are bounded by `start`/`count` and can filter GSP commands, PICA register writes, or shader
instruction offsets. Binary shader and PICA-state exports are explicit chunked operations.
