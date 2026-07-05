"""UART framer + packetiser tests: record reassembly, buffer opcode, timestamp
recovery, and byte-stream resynchronization."""

import struct

import synth
from conftest import packetise

from tilogger.logger import Logger
from tilogger.interface import LogLevel


def formatted(packets):
    """Run packets through the real Log.h formatting layer."""
    lgr = Logger.__new__(Logger)  # format_dobby_packet touches no Logger state
    for p in packets:
        lgr.format_dobby_packet(p)
    return packets


def test_printf_records_reassemble(db):
    stream = synth.log_printf(0x0108, [7, 9], timestamp=5) + synth.log_printf(0x0208, timestamp=6)
    count, boot = formatted(packetise(stream, db))

    assert count.module == "LogMod_App"
    assert count.level == LogLevel.Log_INFO
    assert (count.filename, count.lineno) == ("app.c", "42")
    assert count.data == struct.pack("<HII", 0x0108, 7, 9)
    assert count._str_data == "Count 7 of 9"
    assert count.timestamp == 5

    assert boot.level == LogLevel.Log_WARNING
    assert boot.lineno == "77"
    assert boot.data == struct.pack("<H", 0x0208)
    assert boot._str_data == "Boot complete"
    assert boot.timestamp == 6


def test_buffer_record(db):
    stream = synth.log_buf(0x0308, bytes(range(7)), timestamp=3)
    (buf,) = formatted(packetise(stream, db))
    assert buf.data == struct.pack("<H", 0x0308) + bytes(range(7))
    assert buf._str_data == "buffer: 0x0 0x1 0x2 0x3 0x4 0x5 0x6"
    assert buf.timestamp == 3


def test_resync_after_leading_noise(db):
    """Bytes that are not a valid frame header are discarded until a record starts."""
    noise = bytes([0x00, 0x11, 0x22, 0x33])
    stream = noise + synth.log_printf(0x0108, [1, 2], timestamp=1)
    (count,) = formatted(packetise(stream, db))
    assert count._str_data == "Count 1 of 2"


def test_unknown_id_discarded(db):
    """A record whose id is not in the database must not produce a packet, and
    the stream must resync to the following valid record."""
    stream = synth.log_printf(0xBEEF, [1, 2], timestamp=1) + synth.log_printf(0x0208, timestamp=2)
    packets = formatted(packetise(stream, db))
    assert [p._str_data for p in packets] == ["Boot complete"]


def test_incomplete_record_held(db):
    """A truncated record yields nothing until the rest of the bytes arrive."""
    full = synth.log_printf(0x0108, [7, 9], timestamp=5)
    assert packetise(full[:-1], db) == []
