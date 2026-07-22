# Copyright Citra Emulator Project / Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.

import struct
import random
import enum
import socket
import time

CURRENT_REQUEST_VERSION = 1
MAX_REQUEST_DATA_SIZE = 1024
MAX_PACKET_SIZE = 1024 + 0x10

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

class EmulationState(enum.IntEnum):
    Stopped = 0
    Running = 1
    Paused = 2

CITRA_PORT = 45987

class Citra:
    def __init__(self, address="127.0.0.1", port=CITRA_PORT, timeout=2.0):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.address = address
        self.port = port
        self.socket.settimeout(timeout)

    def is_connected(self):
        return self.socket is not None

    def _generate_header(self, request_type, data_size):
        request_id = random.getrandbits(32)
        return (struct.pack("IIII", CURRENT_REQUEST_VERSION, request_id, request_type, data_size), request_id)

    def _read_and_validate_header(self, raw_reply, expected_id, expected_type):
        reply_version, reply_id, reply_type, reply_data_size = struct.unpack("IIII", raw_reply[:4*4])
        if (CURRENT_REQUEST_VERSION == reply_version and
            expected_id == reply_id and
            expected_type == reply_type and
            reply_data_size == len(raw_reply[4*4:])):
            return raw_reply[4*4:]
        return None

    def _request(self, request_type, data=b""):
        request, request_id = self._generate_header(request_type, len(data))
        self.socket.sendto(request + data, (self.address, self.port))
        raw_reply = self.socket.recv(MAX_PACKET_SIZE)
        return self._read_and_validate_header(raw_reply, request_id, request_type)

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
        if data is None or len(data) != 8:
            return None
        return struct.unpack("II", data)

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

    def pica_timeline(self, start=0xFFFFFFFF, count=20, kind=0xFFFFFFFF,
                      required_changes=0, target_address=0xFFFFFFFF,
                      shader_entry=0xFFFFFFFF, frame=0xFFFFFFFF):
        request = struct.pack("8I", 0, start, count, kind, required_changes,
                              target_address, shader_entry, frame)
        reply = self._request(RequestType.PicaTimeline, request)
        if reply is None or len(reply) < 4:
            return None
        returned = struct.unpack_from("I", reply)[0]
        if len(reply) != 4 + returned * 0x40:
            return None
        return [struct.unpack_from("16I", reply, 4 + index * 0x40)
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
            request_data = struct.pack("II", read_processes, 0x7FFFFFFF)
            request, request_id = self._generate_header(RequestType.ProcessList, len(request_data))
            request += request_data
            self.socket.sendto(request, (self.address, self.port))

            raw_reply = self.socket.recv(MAX_PACKET_SIZE)
            reply_data = self._read_and_validate_header(raw_reply, request_id, RequestType.ProcessList)

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
        request_data = struct.pack("II", 0, 0)
        request, request_id = self._generate_header(RequestType.SetGetProcess, len(request_data))
        request += request_data
        self.socket.sendto(request, (self.address, self.port))

        raw_reply = self.socket.recv(MAX_PACKET_SIZE)
        reply_data = self._read_and_validate_header(raw_reply, request_id, RequestType.SetGetProcess)

        if reply_data:
            return struct.unpack("I", reply_data)[0]
        else:
            return None

    def set_process(self, process_id):
        request_data = struct.pack("II", 1, process_id)
        request, request_id = self._generate_header(RequestType.SetGetProcess, len(request_data))
        request += request_data
        self.socket.sendto(request, (self.address, self.port))

        self.socket.recv(MAX_PACKET_SIZE)

    def read_memory(self, read_address, read_size):
        """
        >>> c.read_memory(0x100000, 4)
        b'\\x07\\x00\\x00\\xeb'
        """
        result = bytes()
        while read_size > 0:
            temp_read_size = min(read_size, MAX_REQUEST_DATA_SIZE)
            request_data = struct.pack("II", read_address, temp_read_size)
            request, request_id = self._generate_header(RequestType.ReadMemory, len(request_data))
            request += request_data
            self.socket.sendto(request, (self.address, self.port))

            raw_reply = self.socket.recv(MAX_PACKET_SIZE)
            reply_data = self._read_and_validate_header(raw_reply, request_id, RequestType.ReadMemory)

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
            request, request_id = self._generate_header(RequestType.WriteMemory, len(request_data))
            request += request_data
            self.socket.sendto(request, (self.address, self.port))

            raw_reply = self.socket.recv(MAX_PACKET_SIZE)
            reply_data = self._read_and_validate_header(raw_reply, request_id, RequestType.WriteMemory)

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
