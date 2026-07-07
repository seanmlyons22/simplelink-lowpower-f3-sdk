"""Packetiser tests: Log_* record reassembly, buffer opcode, timestamp math,
and the stdout/Wireshark regression pins that gate the hot-path refactor."""

import struct

import pytest

import synth
from conftest import FakeLogger, drain, packetise

from tilogger.logger import Logger
from tilogger.interface import LogLevel
from tilogger_itm_transport.itm_to_log import ITMPacketiser

# Framer holds back the trailing <5 bytes of any stream (a partial frame or
# reset token could still arrive); tests append this to flush the real tail.
FLUSH = b"\xff" * 5


def dense_stream() -> bytes:
    s = bytearray(synth.sink_boot())
    s += synth.lts(3000)
    s += synth.log_record(0x9000_0100, [7, 9])
    s += synth.log_record(0x9000_0200)
    s += synth.lts(1500)
    s += synth.log_buf(0x9000_0300, bytes(range(7)))
    s += FLUSH
    return bytes(s)


def formatted(packets):
    """Run packets through the real Log.h formatting layer."""
    lgr = Logger.__new__(Logger)  # format_dobby_packet touches no Logger state
    for p in packets:
        lgr.format_dobby_packet(p)
    return packets


def test_dense_records_reassemble(db):
    packets = formatted(packetise(dense_stream(), db))
    assert len(packets) == 3

    count, boot, buf = packets

    assert count.module == "LogMod_App"
    assert count.level == LogLevel.Log_INFO
    assert (count.filename, count.lineno) == ("app.c", "42")
    assert count.data == struct.pack("<HII", 0x0100, 7, 9)
    assert count._str_data == "Count 7 of 9"

    assert boot.level == LogLevel.Log_WARNING
    assert boot.lineno == "77"
    assert boot.data == struct.pack("<H", 0x0200)
    assert boot._str_data == "Boot complete"

    assert buf.data == struct.pack("<H", 0x0300) + bytes(range(7))
    assert buf._str_data == "buffer: 0x0 0x1 0x2 0x3 0x4 0x5 0x6"


def test_timestamp_math(db):
    """rtc accumulates LTS deltas at clock/prescaler; each non-timestamp frame
    adds its own wire time (size+1 bytes at the TPIU baud rate)."""
    packets = packetise(dense_stream(), db)

    clock, baud, prescaler = 48e6, 12e6, 16  # prescaler set by the boot info frame
    rtc = 3000 / (clock / prescaler)
    # First record header is the frame right after the LTS: one 3-byte id frame.
    expect_first = rtc + 3 / baud
    # Second header follows the first id frame, its two 5-byte trace frames, and
    # then its own 3-byte id frame.
    expect_second = rtc + (3 + 5 + 5 + 3) / baud

    assert packets[0].timestamp_local == pytest.approx(expect_first, abs=1e-12)
    assert packets[1].timestamp_local == pytest.approx(expect_second, abs=1e-12)
    # No global sync in this stream: local and global timestamps coincide.
    assert packets[0].timestamp == packets[0].timestamp_local


def test_incomplete_record_not_emitted(db):
    """A header expecting two args must not complete after only one."""
    s = synth.sink_boot() + synth.log_record(0x9000_0100, [7]) + FLUSH
    assert packetise(s, db) == []


def test_trace_without_header_is_discarded(db):
    s = synth.sink_boot() + synth.sw(synth.STIM_TRACE, struct.pack("<I", 1)) + synth.log_record(0x9000_0200) + FLUSH
    packets = packetise(s, db)
    assert [p._str_data for p in formatted(packets)] == ["Boot complete"]


def test_unknown_header_address_is_discarded(db):
    s = synth.sink_boot() + synth.log_record(0xDEAD_0000, [1, 2]) + synth.log_record(0x9000_0200) + FLUSH
    packets = packetise(s, db)
    assert [p._str_data for p in formatted(packets)] == ["Boot complete"]


# ---------------------------------------------------------------------------
# Output regression pins: exact bytes today, must survive the refactor
# ---------------------------------------------------------------------------

STDOUT_PIN = [
    "ITM0 | 0.001000250 | LogMod_App | Log_INFO | app.c:42 | Count 7 of 9",
    "ITM0 | 0.001001333 | LogMod_App | Log_WARNING | app.c:77 | Boot complete",
]

WIRESHARK_PIN = [
    b"\x00\x00\x00\x00\xe8\x03\x00\x00P\x00\x00\x00P\x00\x00\x00"
    b"ITM0||0.001000250||FORMATTED_TEXT||LogMod_App||Log_INFO||app.c||42||Count 7 of 9",
    b"\x00\x00\x00\x00\xe9\x03\x00\x00T\x00\x00\x00T\x00\x00\x00"
    b"ITM0||0.001001333||FORMATTED_TEXT||LogMod_App||Log_WARNING||app.c||77||Boot complete",
]


def test_stdout_output_pinned(db, capsys):
    from tilogger_stdout.main import StdoutOutput, DEFAULT_LOGGING_SCHEME

    packets = formatted(packetise(dense_stream(), db))[:2]
    out = StdoutOutput(DEFAULT_LOGGING_SCHEME, column_padding=False)
    for p in packets:
        out.notify_packet(p)
    assert capsys.readouterr().out.splitlines() == STDOUT_PIN


def test_wireshark_output_pinned(db):
    from tilogger_wireshark.main import WiresharkOutput

    packets = formatted(packetise(dense_stream(), db))[:2]
    ws = WiresharkOutput(ws_pipe=None)
    for p in packets:
        ws.notify_packet(p)
    # Nothing has connected to the FIFO, so the exact pcap records sit in the
    # backlog: 16-byte record header + ||-delimited payload.
    assert ws._pipe_backlog == WIRESHARK_PIN


# ---------------------------------------------------------------------------
# Surfaced DWT / overflow packets (Task 2)
# ---------------------------------------------------------------------------

from tilogger.dwarf import RangeDict
from tilogger_itm_transport.itm_to_log import (
    DWT_OPCODE_COUNTER_WRAP,
    DWT_OPCODE_EXCEPTION,
    DWT_OPCODE_PC_SAMPLE,
    DWT_OPCODE_WATCHPOINT,
    ITM_OPCODE_OVERFLOW,
    TimestampInfo,
)


def test_pc_sample_unresolved(db):
    s = synth.sink_boot() + synth.lts(3000) + synth.pc_sample(0xDEADBEEF) + FLUSH
    (p,) = packetise(s, db)
    assert p.module == "DWT"
    assert p.opcode == DWT_OPCODE_PC_SAMPLE
    assert p.level == LogLevel.Log_VERBOSE
    assert p._str_data == "pc sample: 0xDEADBEEF"
    assert p.data == struct.pack("<I", 0xDEADBEEF)
    assert (p.filename, p.lineno) == ("dwt", "0")


def test_pc_sample_symbolized(db):
    db.func_ranges = RangeDict({(0x1000, 0x1100): (None, None, "uart_isr", "uart.c", 88)})
    s = synth.sink_boot() + synth.pc_sample(0x1050) + synth.pc_sample(0x10FF) + synth.pc_sample(0x1100) + FLUSH
    inside, last_addr, outside = packetise(s, db)
    assert inside._str_data == "pc sample: 0x00001050 uart_isr (uart.c:88)"
    assert (inside.filename, inside.lineno) == ("uart.c", "88")
    # End address is exclusive: 0x10FF resolves, 0x1100 does not.
    assert last_addr._str_data == "pc sample: 0x000010FF uart_isr (uart.c:88)"
    assert outside._str_data == "pc sample: 0x00001100"


def test_pc_sample_sleep(db):
    s = synth.sink_boot() + synth.pc_sleep() + FLUSH
    (p,) = packetise(s, db)
    assert p._str_data == "pc sample: sleep"


def test_pc_histogram_collects(db):
    from collections import Counter

    db.func_ranges = RangeDict({(0x1000, 0x1100): (None, None, "uart_isr", "uart.c", 88)})
    packetiser = ITMPacketiser(db, FakeLogger())
    packetiser.pc_histogram = Counter()
    s = synth.sink_boot() + synth.pc_sample(0x1050) * 3 + synth.pc_sample(0x2000) + synth.pc_sleep() + FLUSH
    for frame in drain(s):
        packetiser.parse(frame)
    assert packetiser.pc_histogram == {
        ("uart_isr", "uart.c", 88): 3,
        ("0x00002000", None, None): 1,
        ("<sleep>", None, None): 1,
    }


def test_exception_packets(db):
    s = synth.sink_boot()
    s += synth.exception(15, synth.EXC_ENTER)
    s += synth.exception(15, synth.EXC_EXIT)
    s += synth.exception(21, synth.EXC_RETURN)
    s += FLUSH
    enter, exit_, ret = packetise(s, db)
    assert enter.opcode == DWT_OPCODE_EXCEPTION
    assert enter.level == LogLevel.Log_INFO
    assert enter._str_data == "exception 15 (SysTick) entry"
    assert exit_._str_data == "exception 15 (SysTick) exit"
    assert ret._str_data == "exception 21 (IRQ5) return"


def test_watchpoint_packets(db):
    s = synth.sink_boot()
    s += synth.trace_data(1, True, 0x1234)
    s += synth.trace_data(2, False, 0x55)
    s += synth.trace_pc(0, 0x08001234)
    s += synth.trace_address(3, 0x2000)
    s += FLUSH
    wr, rd, pc, addr = packetise(s, db)
    assert wr.opcode == DWT_OPCODE_WATCHPOINT
    assert wr._str_data == "watchpoint 1: write, value 0x1234"
    assert rd._str_data == "watchpoint 2: read, value 0x55"
    assert pc._str_data == "watchpoint 0: PC match, PC 0x08001234"
    assert addr._str_data == "watchpoint 3: address match, addr 0x2000"


def test_counter_wrap_packet(db):
    s = synth.sink_boot() + synth.counter_wrap(0x21) + FLUSH
    (p,) = packetise(s, db)
    assert p.opcode == DWT_OPCODE_COUNTER_WRAP
    assert p._str_data == "counter wrap: CPI Cyc"


def test_overflow_packet(db):
    s = synth.sink_boot() + synth.overflow() + FLUSH
    (p,) = packetise(s, db)
    assert p.module == "ITM"
    assert p.opcode == ITM_OPCODE_OVERFLOW
    assert p.level == LogLevel.Log_WARNING
    assert "dropped" in p._str_data


def test_dwt_packets_share_log_clock(db):
    """DWT packets carry the same accumulated device clock as Log_* records."""
    s = synth.sink_boot()
    s += synth.lts(3000)  # 1 ms at 48 MHz / 16
    s += synth.pc_sample(0x1234)
    s += synth.log_record(0x9000_0200)
    s += FLUSH
    pc, log = packetise(s, db)
    clock, baud, prescaler = 48e6, 12e6, 16
    rtc = 3000 / (clock / prescaler)
    assert pc.timestamp_local == pytest.approx(rtc + 5 / baud, abs=1e-12)
    assert log.timestamp_local == pytest.approx(rtc + (5 + 3) / baud, abs=1e-12)
    assert pc.timestamp_local < log.timestamp_local


# ---------------------------------------------------------------------------
# Timestamp format / resync (STIM_INFO + STIM_SYNC_TIME)
# ---------------------------------------------------------------------------


def test_timestamp_info_from_native_format():
    """LPF3 FreeRTOS nativeFormat64: 8 integer octets, tick = 8 us."""
    word = synth.ts_format_word(0, 8, 6, 8)
    info = TimestampInfo.from_native_format(struct.pack("<I", word))
    assert (info.frac_width, info.int_width) == (0, 64)
    assert (info.exponent, info.multiplier) == (6, 8)


def test_timestamp_info_parse_64bit():
    info = TimestampInfo.from_native_format(struct.pack("<I", synth.ts_format_word(0, 8, 6, 8)))
    ticks = 125_000_000
    assert info.parse_native(ticks & 0xFFFFFFFF) is None  # LSW: waits for MSW
    assert info.parse_native(ticks >> 32) == pytest.approx(1000.0)  # 8 us/tick


def test_timestamp_info_negative_multiplier():
    """Negative multiplier divides: -32768 models a 32 kHz counter."""
    info = TimestampInfo.from_native_format(struct.pack("<I", synth.ts_format_word(0, 4, 0, -32768)))
    assert info.multiplier == -32768
    assert info.parse_native(32768) == pytest.approx(1.0)


def test_timestamp_info_fractional():
    """16.16 fixed point seconds: fracBytes=2, intBytes=2, x1, exp 0."""
    info = TimestampInfo.from_native_format(struct.pack("<I", synth.ts_format_word(2, 2, 0, 1)))
    assert info.parse_native((5 << 16) | 0x8000) == pytest.approx(5.5)


def test_resync_overrides_device_clock(db):
    """A time-sync pair sets the device clock: 125e6 ticks * 8 us = 1000 s."""
    s = synth.sink_boot(native64=125_000_000)
    s += synth.lts(3000)
    s += synth.log_record(0x9000_0200)
    s += FLUSH
    (log,) = packetise(s, db)
    clock, prescaler = 48e6, 16
    expected = 1000.0 + 3000 / (clock / prescaler)
    assert log.timestamp_local == pytest.approx(expected, rel=1e-9)
