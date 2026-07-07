"""Unit tests for the LogSinkBuf transport (streams/buf).

Same style as core/tests/test_bufdecode.py: no fixtures, no mocks beyond a
FakeReader that serves a RingWriter's bytes as if they were target RAM. The
key assertion is "identical to ITM": packets emitted by the transport are run
through the real Logger.format_dobby_packet and must render exactly the text
the decode core produced.
"""

import os
import struct
import sys
from pathlib import Path

import pytest

from tilogger.bufdecode import RingWriter, decode_buffer
from tilogger.interface import LogLevel
from tilogger.logger import Logger
from tilogger.tracedb import ElfString

from tilogger_buf_transport.buf_transport import Buf_Transport, _ticks_factor
from tilogger_buf_transport.memory import DumpReader, MemoryReader

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from bench_buf import WIRE_MBPS_NOMINAL, measure_decode

STRUCT_ADDR = 0x20000000
BUF_ADDR = 0x20000100
INSTANCE = "CONFIG_ti_log_LogSinkBuf_0"


class StubDB:
    """The minimal TraceDB surface the transport and the formatter touch."""

    def __init__(self):
        self.logIndexDB = {}
        self.timestamp_fmt_32 = b""

    def symbol_address(self, name):
        return STRUCT_ADDR if name == "LogSinkBuf_%s_config" % INSTANCE else None

    def symbol_size(self, name):
        return None


DB = StubDB()


def make_elf(fmt, nargs, opcode="LOG_OPCODE_FORMATED_TEXT"):
    """Build an ElfString the way TraceDB does from a .log_data record."""
    value = "\x1e".join([opcode, "file.c", "42", "Log_DEBUG", "LogMod_App", fmt, str(nargs)])
    return ElfString(value, DB)


INDEX = {
    0x0008: make_elf("boot", 0),
    0x000C: make_elf("x=%d", 1),
    0x0010: make_elf("a=%d b=%d", 2),
    0x0018: make_elf("dump ", 0, opcode="LOG_OPCODE_BUFFER"),
}
DB.logIndexDB = INDEX


class FakeReader(MemoryReader):
    """Serve the instance struct and ring bytes from a RingWriter, as if the
    writer's state were live target RAM."""

    def __init__(self, writer):
        self.writer = writer

    def read(self, addr, size):
        w = self.writer
        if addr == STRUCT_ADDR:
            assert size == 28
            return struct.pack("<6IB3x", BUF_ADDR, w.size, w.wr_reserve, w.last_ts, w.rec_count, w.overflow, w.buf_type)
        off = addr - BUF_ADDR
        assert 0 <= off and off + size <= w.size, "read outside the ring"
        return bytes(w.buf[off : off + size])


class FakeLogger:
    def __init__(self):
        self.packets = []

    def get_system_time(self, device_time):
        return device_time

    def log(self, packet):
        self.packets.append(packet)


def make_transport(writer):
    reader = FakeReader(writer)
    transport = Buf_Transport(reader, DB, "buf0", instance=INSTANCE)
    transport._attach()
    return transport, reader


def render(packet):
    """Run the one canonical formatter, exactly as the Logger pipeline does."""
    Logger.format_dobby_packet(None, packet)
    return packet._str_data


# ---------------------------------------------------------------------------
# Struct access
# ---------------------------------------------------------------------------
def test_struct_read_roundtrips_writer_state():
    writer = RingWriter(size=256)
    writer.printf(0x000C, 10, (1,))
    transport, _ = make_transport(writer)

    inst = transport._read_struct()
    assert inst.buffer == BUF_ADDR
    assert inst.size == writer.size
    assert inst.wr_reserve == writer.wr_reserve
    assert inst.last_ts == writer.last_ts
    assert inst.rec_count == writer.rec_count
    assert inst.overflow == writer.overflow
    assert inst.buf_type == writer.buf_type
    assert transport._size == writer.size


def test_attach_rejects_garbage_struct():
    writer = RingWriter(size=256)
    writer.buf_type = 7  # not LINEAR or CIRCULAR: wrong address or wrong image
    with pytest.raises(RuntimeError):
        make_transport(writer)


# ---------------------------------------------------------------------------
# Single poll: emitted packets render identically to the ITM path
# ---------------------------------------------------------------------------
def test_single_poll_packets_match_itm_rendering():
    writer = RingWriter(size=512)
    writes = [
        (0x0008, 10, ()),
        (0x000C, 25, (42,)),
        (0x0010, 900, (1, 0xFFFFFFFF)),
    ]
    for log_id, now, args in writes:
        writer.printf(log_id, now, args)

    transport, _ = make_transport(writer)
    logger = FakeLogger()
    transport._poll_once(logger)

    # Reference decode straight from the core, for the cross-check.
    ref = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)
    assert len(logger.packets) == len(writes) == ref.decoded

    for packet, rec, (log_id, now, _) in zip(logger.packets, ref.records, writes):
        assert packet.module == "LogMod_App"
        assert packet.level == LogLevel.Log_DEBUG
        assert packet.filename == "file.c"
        assert packet.lineno == "42"
        assert packet.timestamp_local == float(now)  # no format word -> raw ticks
        # The "identical to ITM" assertion: the canonical formatter resolves
        # the same id from the same data bytes and renders the same string.
        assert render(packet) == rec.text

    assert render(logger.packets[1]) == "x=42"


def test_buffer_record_renders_identically():
    writer = RingWriter(size=512)
    payload = bytes([0x00, 0x11, 0x00, 0xAB])
    writer.log_buf(0x0018, 30, payload)

    transport, _ = make_transport(writer)
    logger = FakeLogger()
    transport._poll_once(logger)

    ref = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)
    assert len(logger.packets) == 1
    assert render(logger.packets[0]) == ref.records[0].text == "dump 0x0 0x11 0x0 0xab"


# ---------------------------------------------------------------------------
# Multi-poll streaming: rd carry, no double emit
# ---------------------------------------------------------------------------
def test_multi_poll_rd_carry_no_double_emit():
    writer = RingWriter(size=512)
    transport, _ = make_transport(writer)
    logger = FakeLogger()

    writer.printf(0x000C, 10, (1,))
    writer.printf(0x000C, 20, (2,))
    transport._poll_once(logger)
    assert [render(p) for p in logger.packets] == ["x=1", "x=2"]

    transport._poll_once(logger)  # idle poll: nothing new
    assert len(logger.packets) == 2

    writer.printf(0x000C, 30, (3,))
    transport._poll_once(logger)
    assert [render(p) for p in logger.packets] == ["x=1", "x=2", "x=3"]
    assert transport._decoded_prior == writer.rec_count
    assert transport._dropped == 0
    assert transport._torn == 0


def test_lap_overwrite_accounting():
    # Small ring lapped many times between polls: everything committed is
    # either decoded or counted as dropped, never silently lost.
    writer = RingWriter(size=64)
    transport, _ = make_transport(writer)
    logger = FakeLogger()

    for i in range(10):
        writer.printf(0x0010, 100 + i, (i, i))
    transport._poll_once(logger)

    for i in range(200):
        writer.printf(0x0010, 300 + i, (i, i * 2))
    transport._poll_once(logger)

    assert transport._decoded_prior + transport._dropped == writer.rec_count
    assert transport._dropped > 0
    assert transport._torn == 0
    assert render(logger.packets[-1]) == "a=199 b=398"


def test_inflight_frontier_is_not_a_drop():
    writer = RingWriter(size=512)
    for i in range(4):
        writer.printf(0x000C, 10 + i, (i,))
    # A record is reserved (recCount bumped) but its body is mid-write:
    # non-zero bytes past the last delimiter, no terminator yet.
    frontier = writer.wr_reserve
    for k in range(5):
        writer.buf[(frontier + k) % writer.size] = 0x11
    writer.wr_reserve += 5
    writer.rec_count += 1

    transport, _ = make_transport(writer)
    logger = FakeLogger()
    transport._poll_once(logger)

    assert len(logger.packets) == 4
    assert transport._dropped == 0
    assert transport._torn == 0
    assert transport._rd == frontier  # retried next poll, not skipped


def test_torn_frame_resync():
    writer = RingWriter(size=512)
    writer.printf(0x000C, 10, (1,))
    good_end = writer.wr_reserve
    writer.printf(0x0010, 20, (2, 3))
    writer.printf(0x000C, 30, (4,))
    writer.buf[good_end + 1] ^= 0xFF  # corrupt the middle frame

    transport, _ = make_transport(writer)
    logger = FakeLogger()
    transport._poll_once(logger)

    assert transport._torn >= 1
    assert render(logger.packets[-1]) == "x=4"


# ---------------------------------------------------------------------------
# Reboot detection
# ---------------------------------------------------------------------------
def test_reboot_resets_and_resumes():
    writer = RingWriter(size=256)
    for i in range(20):
        writer.printf(0x000C, 10 + i, (i,))
    transport, reader = make_transport(writer)
    logger = FakeLogger()
    transport._poll_once(logger)
    assert transport._decoded_prior > 0

    # Reboot: a fresh image starts the counters over (wrReserve jumps back).
    fresh = RingWriter(size=256)
    fresh.printf(0x000C, 5, (99,))
    reader.writer = fresh
    transport._poll_once(logger)

    assert transport._decoded_prior == 1  # state was cleared, not carried over
    assert transport._dropped == 0
    assert render(logger.packets[-1]) == "x=99"


# ---------------------------------------------------------------------------
# Dump replay path
# ---------------------------------------------------------------------------
def test_dump_reader_decodes_snapshot(tmp_path):
    writer = RingWriter(size=128)
    writer.printf(0x000C, 10, (7,))
    writer.printf(0x0010, 20, (8, 9))

    blob = bytearray(0x100 + writer.size)
    blob[0:28] = struct.pack(
        "<6IB3x", BUF_ADDR, writer.size, writer.wr_reserve, writer.last_ts, writer.rec_count, writer.overflow, writer.buf_type
    )
    blob[0x100:] = writer.buf
    dump = tmp_path / "ram.bin"
    dump.write_bytes(bytes(blob))

    transport = Buf_Transport(DumpReader(dump, STRUCT_ADDR), DB, "buf0", instance=INSTANCE, one_shot=True)
    logger = FakeLogger()
    transport._attach()
    transport._poll_once(logger)
    assert [render(p) for p in logger.packets] == ["x=7", "a=8 b=9"]


def test_dump_reader_out_of_range_is_a_clear_error(tmp_path):
    dump = tmp_path / "ram.bin"
    dump.write_bytes(b"\x00" * 64)
    reader = DumpReader(dump, STRUCT_ADDR)
    with pytest.raises(ValueError, match="outside the dump"):
        reader.read(STRUCT_ADDR + 60, 8)


# ---------------------------------------------------------------------------
# Timestamp conversion (TimestampP native format word)
# ---------------------------------------------------------------------------
def test_ticks_factor():
    def word(multiplier, exponent, int_bytes, frac_bytes):
        return struct.pack("<I", ((multiplier & 0xFFFF) << 16) | (exponent << 8) | (int_bytes << 4) | frac_bytes)

    assert _ticks_factor(b"") == 1.0  # no symbol: raw ticks
    assert _ticks_factor(word(1, 3, 4, 0)) == pytest.approx(1e-3)  # 1 kHz tick
    assert _ticks_factor(word(-32768, 0, 2, 2)) == pytest.approx(1.0 / (32768 * 65536))  # 32 kHz RTC, 16.16
    transport = Buf_Transport(FakeReader(RingWriter(size=64)), DB, "buf0")
    assert transport.timestamp_to_seconds(1500) == 1500.0


# ---------------------------------------------------------------------------
# Performance: decode must beat the wire (see scripts/bench_buf.py)
# ---------------------------------------------------------------------------
def test_decode_faster_than_the_wire():
    decode_mbps, _, decoded = measure_decode(ring_bytes=1 << 20)
    assert decoded > 0
    assert decode_mbps > WIRE_MBPS_NOMINAL, (
        "decode (%.2f MB/s) no longer clears the nominal SWD read rate "
        "(%.2f MB/s); decode is the bottleneck now" % (decode_mbps, WIRE_MBPS_NOMINAL)
    )


# ---------------------------------------------------------------------------
# HIL smoke (needs a board and pyOCD): TILOGGER_BUF_HIL=1 TILOGGER_BUF_ELF=app.out
# ---------------------------------------------------------------------------
@pytest.mark.skipif(os.environ.get("TILOGGER_BUF_HIL") != "1", reason="hardware-in-the-loop test")
def test_hil_attach_to_running_device():
    from tilogger.tracedb import TraceDB

    from tilogger_buf_transport.memory import PyocdReader

    elf = os.environ.get("TILOGGER_BUF_ELF")
    assert elf, "set TILOGGER_BUF_ELF to the running application's .out file"

    db = TraceDB([elf], repickle=False)
    transport = Buf_Transport(PyocdReader(), db, "hil", instance=os.environ.get("TILOGGER_BUF_INSTANCE", INSTANCE))
    logger = FakeLogger()
    transport._attach()
    for _ in range(50):
        transport._poll_once(logger)
    transport.stop()

    # The tally must stay sane even if the app logged nothing during the poll.
    assert transport._decoded_prior == len(logger.packets)
    assert transport._decoded_prior + transport._dropped <= transport._prev_rec


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
