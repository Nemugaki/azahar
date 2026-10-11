# Copyright Citra Emulator Project / Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the misc/licenses/gplv2.txt file included.

import struct
import random
import enum
import socket
import time

PROTOCOL_MAGIC = 0x525A4841     # "AHZR"
ERROR_REPLY_MAGIC = 0x52504345  # "ECPR"
HEADER_SIZE = 0x10
MAX_REQUEST_DATA_SIZE = 32 * 1024
MAX_PACKET_SIZE = HEADER_SIZE + MAX_REQUEST_DATA_SIZE

class RequestType(enum.IntEnum):
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
    TakeScreenshot = 20,
    ReadScreenshot = 21,
    GetPerfStats = 22,

class Error(enum.IntEnum):
    InvalidPacket = 1
    PermissionDenied = 2
    InvalidState = 3
    InvalidArgument = 4
    Unsupported = 5
    NotFound = 6
    Busy = 7
    Failed = 8

class RPCError(Exception):
    def __init__(self, error):
        self.error = Error(error) if error in Error._value2member_map_ else error
        super().__init__(f"RPC request failed: {self.error!r}")

class ScreenshotResult(enum.IntEnum):
    Success = 0
    Unsupported = 1
    Busy = 2
    Timeout = 3
    EncodeFailed = 4
    InvalidArgument = 5

SCREENSHOT_FLAG_SECONDARY_WINDOW = 1 << 0
SCREENSHOT_FLAG_RAW_RGB = 1 << 1

PERF_STATS_FIELDS = ("system_fps", "game_fps", "time_vblank_interval", "time_hle_svc",
                     "time_hle_ipc", "time_gpu", "time_swap", "time_remaining",
                     "emulation_speed", "artic_transmitted", "artic_events")

class EmulationControl(enum.IntEnum):
    Status = 0
    Run = 1
    Pause = 2
    Resume = 3
    Stop = 4
    Restart = 5
    DebugPause = 6
    DebugResume = 7
    SaveState = 8
    LoadState = 9
    Screenshot = 10
    FrameAdvance = 11

class RenderDebugOperation(enum.IntEnum):
    Capabilities = 0
    CaptureFrame = 1
    Timeline = 2
    State = 3
    Validate = 4
    OutputStatus = 5
    OutputRead = 6
    Diff = 7
    Vertex = 8
    DrawBreak = 9

class EmulationState(enum.IntEnum):
    Stopped = 0
    Running = 1
    Paused = 2

CITRA_PORT = 45987

TIMELINE_ENTRY_SIZE = 0x44
TIMELINE_ENTRY_FIELDS = ("sequence", "kind", "frame", "draw", "frame_draw", "changed_mask",
                         "color_address", "depth_address", "width", "height", "color_format",
                         "depth_format", "draw_mode", "vertex_count", "topology",
                         "vertex_offset", "vertex_shader_entry")

def _timeline_entry(data, offset):
    return dict(zip(TIMELINE_ENTRY_FIELDS, struct.unpack_from("<17I", data, offset)))

class Citra:
    def __init__(self, address="127.0.0.1", port=CITRA_PORT, timeout=2.0, retries=3):
        """
        Requests that time out are resent up to `retries` times with the same id; the server
        answers duplicates from its reply cache instead of running them again.
        """
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for option in (socket.SO_SNDBUF, socket.SO_RCVBUF):
            if self.socket.getsockopt(socket.SOL_SOCKET, option) < MAX_PACKET_SIZE * 2:
                self.socket.setsockopt(socket.SOL_SOCKET, option, MAX_PACKET_SIZE * 2)
        self.address = address
        self.port = port
        self.timeout = timeout
        self.retries = retries

    def is_connected(self):
        return self.socket is not None

    def _generate_header(self, request_type, data_size):
        request_id = random.getrandbits(32)
        return (struct.pack("<IIII", PROTOCOL_MAGIC, request_id, request_type, data_size), request_id)

    def _read_and_validate_header(self, raw_reply, expected_id, expected_type):
        if len(raw_reply) < HEADER_SIZE:
            return None
        magic, reply_id, reply_type, reply_data_size = struct.unpack("<IIII", raw_reply[:HEADER_SIZE])
        if (magic != PROTOCOL_MAGIC or reply_id != expected_id or reply_type != expected_type or
                reply_data_size != len(raw_reply) - HEADER_SIZE):
            return None
        data = raw_reply[HEADER_SIZE:]
        if len(data) == 8 and struct.unpack_from("<I", data)[0] == ERROR_REPLY_MAGIC:
            raise RPCError(struct.unpack_from("<I", data, 4)[0])
        return data

    def _request(self, request_type, data=b"", timeout=None, resend=True):
        """Returns the reply data, raises RPCError on an error reply and TimeoutError if the
        server never answers."""
        request, request_id = self._generate_header(request_type, len(data))
        self.socket.settimeout(self.timeout if timeout is None else timeout)
        attempts = self.retries + 1 if resend else 1
        for _ in range(attempts):
            self.socket.sendto(request + data, (self.address, self.port))
            try:
                while True:
                    reply = self._read_and_validate_header(self.socket.recv(MAX_PACKET_SIZE),
                                                           request_id, request_type)
                    if reply is not None:
                        return reply
            except socket.timeout:
                continue
        raise TimeoutError(f"No reply to {RequestType(request_type).name} after {attempts} attempts")

    def _read_chunks(self, request_type, operation, generation, total_size):
        result = bytearray()
        while len(result) < total_size:
            size = min(MAX_REQUEST_DATA_SIZE, total_size - len(result))
            request = struct.pack("IIII", operation, generation, len(result), size)
            chunk = self._request(request_type, request)
            if not chunk:
                return None
            result.extend(chunk)
        return bytes(result)

    def capabilities(self):
        data = self._request(RequestType.Capabilities)
        if len(data) < 16:
            return None
        magic, capabilities, max_data_size, build_size = struct.unpack_from("<4I", data)
        if magic != PROTOCOL_MAGIC or len(data) != 16 + build_size:
            return None
        return {
            "capabilities": capabilities,
            "max_packet_data_size": max_data_size,
            "build": data[16:].decode("utf-8", errors="replace"),
        }

    def emulation_control(self, operation, path=""):
        data = struct.pack("I", operation) + path.encode("utf-8")
        reply = self._request(RequestType.EmulationControl, data)
        if reply is None or len(reply) != 8:
            return None
        result, state = struct.unpack("II", reply)
        return result, EmulationState(state)

    def status(self):
        return self.emulation_control(EmulationControl.Status)

    def run(self, path):
        return self.emulation_control(EmulationControl.Run, path)

    def pause(self):
        return self.emulation_control(EmulationControl.Pause)

    def resume(self):
        return self.emulation_control(EmulationControl.Resume)

    def stop(self):
        return self.emulation_control(EmulationControl.Stop)

    def restart(self):
        return self.emulation_control(EmulationControl.Restart)

    def debug_pause(self):
        return self.emulation_control(EmulationControl.DebugPause)

    def debug_resume(self):
        return self.emulation_control(EmulationControl.DebugResume)

    def save_state(self, slot):
        return self.emulation_control(EmulationControl.SaveState, str(slot))

    def load_state(self, slot):
        return self.emulation_control(EmulationControl.LoadState, str(slot))

    def screenshot(self, path):
        return self.emulation_control(EmulationControl.Screenshot, str(path))

    def frame_advance(self):
        return self.emulation_control(EmulationControl.FrameAdvance)

    def _take_screenshot(self, res_scale, flags):
        # The server replies once a frame has been rendered (or after its 10 second timeout).
        # Resending would start a second capture, so the request is sent only once.
        reply = self._request(RequestType.TakeScreenshot, struct.pack("<II", res_scale, flags),
                              timeout=max(self.timeout or 0, 15), resend=False)
        result, width, height, data_size = struct.unpack("<4I", reply[:16])
        if result != ScreenshotResult.Success:
            raise RuntimeError(f"Screenshot failed: {ScreenshotResult(result).name}")
        data = bytearray()
        while len(data) < data_size:
            size = min(MAX_REQUEST_DATA_SIZE, data_size - len(data))
            chunk = self._request(RequestType.ReadScreenshot, struct.pack("<II", len(data), size))
            if not chunk:
                raise RuntimeError("Screenshot data is no longer available")
            data.extend(chunk)
        return width, height, bytes(data)

    def screenshot_png(self, res_scale=0, secondary_window=False):
        """Returns the current frame as PNG bytes; res_scale 0 uses the internal resolution."""
        flags = SCREENSHOT_FLAG_SECONDARY_WINDOW if secondary_window else 0
        return self._take_screenshot(res_scale, flags)[2]

    def screenshot_raw(self, res_scale=0, secondary_window=False):
        """Returns (width, height, rgb8_bytes) for the current frame."""
        flags = SCREENSHOT_FLAG_RAW_RGB
        if secondary_window:
            flags |= SCREENSHOT_FLAG_SECONDARY_WINDOW
        return self._take_screenshot(res_scale, flags)

    def perf_stats(self, reset=False):
        """Returns the frontend's performance stats; reset=True computes and resets them."""
        reply = self._request(RequestType.GetPerfStats, struct.pack("<I", 1 if reset else 0))
        return dict(zip(PERF_STATS_FIELDS, struct.unpack("<10dI", reply[:0x54])))

    def pica_timeline(self, start=0xFFFFFFFF, count=20, kind=0xFFFFFFFF,
                      required_changes=0, target_address=0xFFFFFFFF,
                      shader_entry=0xFFFFFFFF, frame=0xFFFFFFFF):
        request = struct.pack("8I", 0, start, count, kind, required_changes,
                              target_address, shader_entry, frame)
        reply = self._request(RequestType.PicaTimeline, request)
        if reply is None or len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * TIMELINE_ENTRY_SIZE:
            return None
        return [_timeline_entry(reply, 4 + index * TIMELINE_ENTRY_SIZE)
                for index in range(returned)]

    def pica_timeline_status(self):
        reply = self._request(RequestType.PicaTimeline, struct.pack("I", 2))
        return struct.unpack("4I", reply) if reply and len(reply) == 16 else None

    def clear_pica_timeline(self):
        return self._request(RequestType.PicaTimeline, struct.pack("I", 1)) is not None

    def pica_render_target(self):
        reply = self._request(RequestType.PicaRenderTarget, struct.pack("I", 0))
        return struct.unpack("6I", reply) if reply and len(reply) == 24 else None

    def render_sessions(self, start=0, count=15):
        reply = self._request(RequestType.RenderSession,
                              struct.pack("5I", 0, 0, 0, start, count))
        if reply is None or len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * 0x40:
            return None
        sessions = []
        for index in range(returned):
            session_id, capabilities, flags, producer, backend = struct.unpack_from(
                "QQI20s20s", reply, 4 + index * 0x40)
            sessions.append({
                "id": session_id,
                "capabilities": capabilities,
                "live": bool(flags & 1),
                "active": bool(flags & 2),
                "complete": bool(flags & 4),
                "truncated": bool(flags & 8),
                "producer": producer.rstrip(b"\0").decode("utf-8", errors="replace"),
                "backend": backend.rstrip(b"\0").decode("utf-8", errors="replace"),
            })
        return sessions

    def render_session(self, operation, session_id=0, path=""):
        request = struct.pack("5I", operation, session_id & 0xFFFFFFFF,
                              session_id >> 32, 0, 0) + str(path).encode("utf-8")
        reply = self._request(RequestType.RenderSession, request)
        if reply is None or len(reply) != 0x40:
            return None
        result_id, capabilities, flags, producer, backend = struct.unpack_from(
            "QQI20s20s", reply)
        return {
            "id": result_id,
            "capabilities": capabilities,
            "live": bool(flags & 1),
            "active": bool(flags & 2),
            "complete": bool(flags & 4),
            "truncated": bool(flags & 8),
            "producer": producer.rstrip(b"\0").decode("utf-8", errors="replace"),
            "backend": backend.rstrip(b"\0").decode("utf-8", errors="replace"),
        }

    def select_render_session(self, session_id):
        return self.render_session(1, session_id)

    def remove_render_session(self, session_id):
        return self.render_session(2, session_id)

    def import_render_capture(self, path):
        return self.render_session(3, path=path)

    def export_render_capture(self, path, session_id=0):
        return self.render_session(4, session_id, path)

    def render_output_status(self, sequence, session_id=0):
        request = struct.pack("4I", 0, session_id & 0xFFFFFFFF,
                              session_id >> 32, sequence)
        reply = self._request(RequestType.RenderOutput, request)
        if reply is None or len(reply) != 0x28:
            return None
        result_id, result_sequence, format_, address, width, height, stride, size = \
            struct.unpack("<QIIQIIII", reply)
        return {
            "session_id": result_id,
            "sequence": result_sequence,
            "format": format_,
            "address": address,
            "width": width,
            "height": height,
            "stride": stride,
            "size": size,
        }

    def render_output(self, sequence, session_id=0):
        status = self.render_output_status(sequence, session_id)
        if status is None:
            return None
        result = bytearray()
        while len(result) < status["size"]:
            size = min(MAX_REQUEST_DATA_SIZE, status["size"] - len(result))
            request = struct.pack("6I", 1, status["session_id"] & 0xFFFFFFFF,
                                  status["session_id"] >> 32, sequence,
                                  len(result), size)
            chunk = self._request(RequestType.RenderOutput, request)
            if chunk is None or len(chunk) != size:
                return None
            result.extend(chunk)
        status["bytes"] = bytes(result)
        return status

    # Render debugger (RequestType.RenderDebug). Session id 0 means the active session.

    def _render_debug(self, operation, words=(), extra=b"", **kwargs):
        request = struct.pack(f"<{1 + len(words)}I", operation, *words) + extra
        return self._request(RequestType.RenderDebug, request, **kwargs)

    @staticmethod
    def _session_words(session_id):
        return (session_id & 0xFFFFFFFF, session_id >> 32)

    def render_debug_capabilities(self):
        reply = self._render_debug(RenderDebugOperation.Capabilities)
        capabilities, max_data_size, build_size = struct.unpack_from("<QII", reply)
        return {
            "capture_capabilities": capabilities,
            "max_packet_data_size": max_data_size,
            "build": reply[16:16 + build_size].decode("utf-8", errors="replace"),
        }

    def capture_render_frame(self, path, timeout_ms=10000):
        """Captures one complete frame to `path` and imports it as a new session."""
        # Long-running and not idempotent while in progress, so it is sent once.
        reply = self._render_debug(RenderDebugOperation.CaptureFrame, (timeout_ms,),
                                   str(path).encode("utf-8"),
                                   timeout=timeout_ms / 1000 + 5, resend=False)
        session_id, timeline_count, issue_count, complete, truncated = \
            struct.unpack("<QIIII", reply)
        return {"session_id": session_id, "timeline_count": timeline_count,
                "issue_count": issue_count, "complete": bool(complete),
                "truncated": bool(truncated)}

    def render_timeline(self, session_id=0, epoch=0, start=0, count=64, frame=0xFFFFFFFF):
        reply = self._render_debug(RenderDebugOperation.Timeline,
                                   (*self._session_words(session_id), epoch & 0xFFFFFFFF,
                                    epoch >> 32, start, count, frame))
        epoch_low, epoch_high, next_sequence, returned, flags = struct.unpack_from("<5I", reply)
        if len(reply) != 0x14 + returned * TIMELINE_ENTRY_SIZE:
            return None
        return {
            "epoch": epoch_low | epoch_high << 32,
            "next_sequence": next_sequence,
            "has_more": bool(flags & 1),
            "stale": bool(flags & 2),
            "entries": [_timeline_entry(reply, 0x14 + index * TIMELINE_ENTRY_SIZE)
                        for index in range(returned)],
        }

    def render_state(self, sequence, session_id=0, start=0, count=0xFFFFFFFF):
        """Returns (total, start, register_words) for the draw's PICA register state."""
        reply = self._render_debug(RenderDebugOperation.State,
                                   (*self._session_words(session_id), sequence, start, count))
        total, first, returned = struct.unpack_from("<3I", reply)
        return total, first, struct.unpack_from(f"<{returned}I", reply, 12)

    def render_validate(self, session_id=0, strict=True, start=0, count=64):
        reply = self._render_debug(RenderDebugOperation.Validate,
                                   (*self._session_words(session_id), int(strict), start, count))
        returned = struct.unpack_from("<I", reply)[0]
        issues = []
        for index in range(returned):
            kind, frame, draw, role, slot, message = struct.unpack_from(
                "<5I76s", reply, 4 + index * 0x60)
            issues.append({"kind": kind, "frame": frame, "draw": draw, "role": role,
                           "slot": slot,
                           "message": message.rstrip(b"\0").decode("utf-8", errors="replace")})
        return issues

    def render_draw_output_status(self, sequence, phase=2, session_id=0):
        """phase: 0 = draw input, 1 = pre-draw, 2 = post-draw."""
        reply = self._render_debug(RenderDebugOperation.OutputStatus,
                                   (*self._session_words(session_id), sequence, phase, 0))
        (result_session, result_sequence, result_phase, address, format_, width, height,
         stride, tiling, origin, size, _) = struct.unpack("<QIIQ8I", reply)
        return {"session_id": result_session, "sequence": result_sequence,
                "phase": result_phase, "address": address, "format": format_,
                "width": width, "height": height, "stride": stride, "tiling": tiling,
                "origin": origin, "size": size}

    def render_draw_output(self, sequence, phase=2, session_id=0):
        status = self.render_draw_output_status(sequence, phase, session_id)
        data = bytearray()
        while len(data) < status["size"]:
            chunk = self._render_debug(RenderDebugOperation.OutputRead,
                                       (*self._session_words(status["session_id"]), sequence,
                                        phase, len(data)))
            if not chunk:
                return None
            data.extend(chunk)
        status["bytes"] = bytes(data)
        return status

    def render_diff(self, left_sequence, right_sequence, session_id=0, other_session_id=0):
        reply = self._render_debug(RenderDebugOperation.Diff,
                                   (*self._session_words(session_id),
                                    *self._session_words(other_session_id),
                                    left_sequence, right_sequence))
        fields = ("changed_mask", "changed_registers", "changed_resources", "changed_pixels",
                  "min_x", "min_y", "max_x", "max_y")
        return dict(zip(fields, struct.unpack("<8I", reply)))

    def render_vertex(self, sequence, ordinal, session_id=0):
        """Re-runs the vertex shader for one vertex of a captured draw."""
        reply = self._render_debug(RenderDebugOperation.Vertex,
                                   (*self._session_words(session_id), sequence, ordinal))
        (ordinal_, resolved, loaded_mask, fixed_mask, shader_entry, _,
         shader_id) = struct.unpack_from("<6IQ", reply)
        input_map = list(reply[0x20:0x30])
        floats = struct.unpack_from("<152f", reply, 0x30)
        return {"ordinal": ordinal_, "resolved_index": resolved, "loaded_mask": loaded_mask,
                "fixed_mask": fixed_mask, "shader_entry": shader_entry, "shader_id": shader_id,
                "input_map": input_map,
                "input": [floats[i:i + 4] for i in range(0, 64, 4)],
                "output": [floats[64 + i:64 + i + 4] for i in range(0, 64, 4)],
                "semantic": floats[128:152]}

    def render_draw_break(self, command=1, frame_draw=0, color_address=None):
        """command: 0 = arm at frame_draw (optionally only for color_address), 1 = status,
        2 = cancel. Returns the break state."""
        words = (command, frame_draw, color_address or 0, int(color_address is not None))
        reply = self._render_debug(RenderDebugOperation.DrawBreak, words)
        return dict(zip(("phase", "frame_draw", "color_address", "filter_color"),
                        struct.unpack("<4I", reply)))

    def pica_snapshot_status(self):
        reply = self._request(RequestType.PicaSnapshot, struct.pack("I", 1))
        if reply is None or len(reply) != 8:
            return None
        return struct.unpack("II", reply)

    def capture_pica_snapshot(self, timeout=2.0):
        reply = self._request(RequestType.PicaSnapshot, struct.pack("I", 0))
        if reply is None or len(reply) != 8:
            return None
        previous_generation, _ = struct.unpack("II", reply)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.pica_snapshot_status()
            if status and status[0] != previous_generation:
                generation, total_size = status
                return self._read_chunks(RequestType.PicaSnapshot, 2, generation, total_size)
            time.sleep(0.01)
        return None

    def pica_breakpoints(self):
        reply = self._request(RequestType.PicaBreakpoint, struct.pack("I", 0))
        if reply is None or len(reply) != 12:
            return None
        return struct.unpack("III", reply)

    def set_pica_breakpoint(self, event, enabled=True):
        reply = self._request(RequestType.PicaBreakpoint,
                              struct.pack("III", 1, event, enabled))
        return struct.unpack("III", reply) if reply and len(reply) == 12 else None

    def resume_pica_breakpoint(self):
        reply = self._request(RequestType.PicaBreakpoint, struct.pack("I", 2))
        return struct.unpack("III", reply) if reply and len(reply) == 12 else None

    def clear_pica_breakpoints(self):
        reply = self._request(RequestType.PicaBreakpoint, struct.pack("I", 3))
        return struct.unpack("III", reply) if reply and len(reply) == 12 else None

    def pica_trace_status(self):
        reply = self._request(RequestType.PicaTrace, struct.pack("I", 0))
        return struct.unpack("III", reply) if reply and len(reply) == 12 else None

    def start_pica_trace(self):
        reply = self._request(RequestType.PicaTrace, struct.pack("I", 1))
        return struct.unpack("III", reply) if reply and len(reply) == 12 else None

    def finish_pica_trace(self):
        reply = self._request(RequestType.PicaTrace, struct.pack("I", 2))
        if reply is None or len(reply) != 12:
            return None
        _, generation, total_size = struct.unpack("III", reply)
        return self._read_chunks(RequestType.PicaTrace, 3, generation, total_size)

    def pica_trace_writes(self, generation, start=0, count=20, register_id=0xFFFFFFFF):
        request = struct.pack("IIIII", 5, generation, start, count, register_id)
        reply = self._request(RequestType.PicaTrace, request)
        if reply is None or len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * 8:
            return None
        return [struct.unpack_from("HHI", reply, 4 + index * 8) for index in range(returned)]

    def cpu_registers(self, bank, start=0, count=1):
        reply = self._request(RequestType.CPURegisters, struct.pack("III", bank, start, count))
        if not reply or len(reply) % 4:
            return None
        return struct.unpack("I" * (len(reply) // 4), reply)

    def gx_command_trace(self, operation=0, start=0, count=0, command_id=0xFFFFFFFF):
        request = struct.pack("IIII", operation, start, count, command_id)
        reply = self._request(RequestType.GXCommandTrace, request)
        if reply is None:
            return None
        if operation != 3:
            return struct.unpack("II", reply) if len(reply) == 8 else None
        if len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * 0x20:
            return None
        return [struct.unpack_from("8I", reply, 4 + index * 0x20)
                for index in range(returned)]

    def pica_shader_status(self, operation=0):
        reply = self._request(RequestType.PicaShader, struct.pack("I", operation))
        return struct.unpack("IIII", reply) if reply and len(reply) == 16 else None

    def pica_shader_dump(self, generation, total_size):
        return self._read_chunks(RequestType.PicaShader, 2, generation, total_size)

    def pica_shader_cycles(self, generation, start=0, count=8,
                           instruction_offset=0xFFFFFFFF):
        record_size = 0x70
        request = struct.pack("IIIII", 3, generation, start, count, instruction_offset)
        reply = self._request(RequestType.PicaShader, request)
        if reply is None or len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * record_size:
            return None
        return [struct.unpack_from("IIII20I2iII", reply, 4 + index * record_size)
                for index in range(returned)]

    def process_list(self):
        processes = {}
        read_processes = 0
        while True:
            reply_data = self._request(RequestType.ProcessList,
                                       struct.pack("II", read_processes, 0x7FFFFFFF))

            if reply_data:
                read_count = struct.unpack("I", reply_data[0:4])[0]
                reply_data = reply_data[4:]
                if read_count == 0:
                    break
                read_processes += read_count
                for i in range(read_count):
                    proc_data = reply_data[i * 0x14 : (i + 1) * 0x14]
                    proc_id, title_id, proc_name = struct.unpack("<IQ8s", proc_data)
                    proc_name = proc_name.rstrip(b"\x00").decode("ascii")
                    processes[proc_id] = (title_id, proc_name)
            else:
                break
        return processes

    def get_process(self):
        reply_data = self._request(RequestType.SetGetProcess, struct.pack("II", 0, 0))
        if reply_data:
            return struct.unpack("I", reply_data)[0]
        else:
            return None

    def set_process(self, process_id):
        self._request(RequestType.SetGetProcess, struct.pack("II", 1, process_id))

    def read_memory(self, read_address, read_size):
        """
        >>> c.read_memory(0x100000, 4)
        b'\\x07\\x00\\x00\\xeb'
        """
        result = bytes()
        while read_size > 0:
            temp_read_size = min(read_size, MAX_REQUEST_DATA_SIZE)
            reply_data = self._request(RequestType.ReadMemory,
                                       struct.pack("II", read_address, temp_read_size))

            if reply_data:
                result += reply_data
                read_size -= len(reply_data)
                read_address += len(reply_data)
            else:
                return None

        return result

    def write_memory(self, write_address, write_contents):
        """
        >>> c.write_memory(0x100000, b"\\xff\\xff\\xff\\xff")
        True
        >>> c.read_memory(0x100000, 4)
        b'\\xff\\xff\\xff\\xff'
        >>> c.write_memory(0x100000, b"\\x07\\x00\\x00\\xeb")
        True
        >>> c.read_memory(0x100000, 4)
        b'\\x07\\x00\\x00\\xeb'
        """
        write_size = len(write_contents)
        while write_size > 0:
            temp_write_size = min(write_size, MAX_REQUEST_DATA_SIZE - 8)
            request_data = struct.pack("II", write_address, temp_write_size)
            request_data += write_contents[:temp_write_size]
            reply_data = self._request(RequestType.WriteMemory, request_data)

            if None != reply_data:
                write_address += temp_write_size
                write_size -= temp_write_size
                write_contents = write_contents[temp_write_size:]
            else:
                return False
        return True

if "__main__" == __name__:
    import doctest
    doctest.testmod(extraglobs={'c': Citra()})
