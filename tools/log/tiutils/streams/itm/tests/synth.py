"""Synthetic ITM/SWO byte-stream encoder for tests and benchmarks.

Every encoder here is written from the ARM specs, not from the decoder under
test, so a round-trip through the framer/packetiser is a real conformance
check and not a tautology:

- ITM protocol packets:  ARMv7-M ARM (ARM DDI 0403), Appendix D4 "Debug ITM
  and DWT Packet Protocol"; identical encodings in ARMv8-M ARM (ARM DDI 0553)
  for the packets the TI sinks emit.
- TI Log_* framing on top of ITM stimulus ports: source/ti/log/LogSinkITM.c
  (STIM_HEADER carries the .log_data record address, STIM_TRACE carries
  argument words / buffer length + payload).

Header encodings (from DDI 0403 D4.2):
  SW source  : (port << 3) | size_code                 bit2 = 0
  HW source  : (disc << 3) | 0x04 | size_code          bit2 = 1
  size_code  : 1 byte = 0b01, 2 bytes = 0b10, 4 bytes = 0b11
  LTS1       : 0b1TTT0000, T = relation code, LEB128-style payload
  LTS2       : 0b0TTT0000, T = timestamp value 1..6, no payload
  GTS1/GTS2  : 0x94 / 0xB4, LEB128-style payload
  Sync       : at least 47 zero bytes followed by 0x80 (the framer accepts
               the historical 0x01 terminator; see test_framer)
  Overflow   : 0x70
  Extension  : 0b0xxx1S00 (S=0 for ITM extension), continuation via bit 7
"""

from __future__ import annotations

import struct
import zlib
from typing import Iterable

# TI stimulus port allocation, from LogSinkITM.h
STIM_TRACE = 28
STIM_HEADER = 29
STIM_SYNC_TIME = 30
STIM_INFO = 31

# The sink announces a device reset by an STIM_INFO word of 0xBBBBBBBB
# (LogSinkITM.c LogSinkITM_RESET_FRAME). On the wire that is 0xFB BB BB BB BB.
RESET = bytes([0xFB, 0xBB, 0xBB, 0xBB, 0xBB])

_SIZE_CODE = {1: 0b01, 2: 0b10, 4: 0b11}


def sw(port: int, payload: bytes) -> bytes:
    """Software (stimulus) source packet."""
    return bytes([(port << 3) | _SIZE_CODE[len(payload)]]) + bytes(payload)


def hw(disc: int, payload: bytes) -> bytes:
    """Hardware (DWT) source packet with the given discriminator."""
    return bytes([(disc << 3) | 0x04 | _SIZE_CODE[len(payload)]]) + bytes(payload)


def _leb(value: int, max_bytes: int) -> bytes:
    """LEB128-style continuation payload used by LTS1 and GTS packets."""
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value and len(out) + 1 < max_bytes:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def lts(cycles: int, code: int = 0xC) -> bytes:
    """Local timestamp packet, format 1 (header + payload).

    code 0xC = timestamp synchronous to data, 0xD = timestamp delayed,
    0xE = packet delayed, 0xF = packet and timestamp delayed.
    """
    return bytes([0x80 | (code << 4)]) + _leb(cycles, 4)


def lts2(delta: int) -> bytes:
    """Local timestamp packet, format 2: single byte, delta 1..6."""
    if not 1 <= delta <= 6:
        raise ValueError("LTS2 encodes only deltas 1..6")
    return bytes([delta << 4])


def gts1(ts_low26: int, wrap: bool = False, clk_change: bool = False) -> bytes:
    """Global timestamp packet 1: header 0x94, TS bits [25:0] + wrap/clkch."""
    value = ts_low26 & 0x03FF_FFFF
    if clk_change:
        value |= 1 << 26
    if wrap:
        value |= 1 << 27
    return bytes([0x94]) + _leb(value, 4)


def gts2(ts_high: int, bits64: bool = False) -> bytes:
    """Global timestamp packet 2: header 0xB4, TS bits [47:26] or [63:26]."""
    return bytes([0xB4]) + _leb(ts_high, 7 if bits64 else 4)


def sync_packet(zeros: int = 47, terminator: int = 0x01) -> bytes:
    """Synchronization packet. The ARM encoding ends in 0x80; the framer
    historically scans for a 0x01 terminator, so tests can pin either."""
    return bytes(zeros) + bytes([terminator])


def overflow() -> bytes:
    return bytes([0x70])


def extension(page: int) -> bytes:
    """Single-byte stimulus page extension packet (DDI 0403 D4.2.6)."""
    return bytes([((page & 0x7) << 4) | 0x08])


# ---------------------------------------------------------------------------
# DWT hardware event packets (DDI 0403 D4.3)
# ---------------------------------------------------------------------------

# Exception trace function codes (payload byte 1, bits [5:4])
EXC_ENTER = 1
EXC_EXIT = 2
EXC_RETURN = 3


def pc_sample(pc: int) -> bytes:
    """Periodic PC sample packet (discriminator 2, 4-byte payload)."""
    return hw(0x02, struct.pack("<I", pc))


def pc_sleep() -> bytes:
    """Periodic PC sample, sleep variant (1 zero byte payload)."""
    return hw(0x02, b"\x00")


def exception(num: int, fn: int) -> bytes:
    """Exception trace packet (discriminator 1, 2-byte payload)."""
    return hw(0x01, bytes([num & 0xFF, ((num >> 8) & 0x1) | (fn << 4)]))


def counter_wrap(mask: int) -> bytes:
    """Event counter wrap packet (discriminator 0, 1-byte payload).

    mask bits: 0=Cyc 1=Fold 2=LSU 3=Sleep 4=Exc 5=CPI.
    """
    return hw(0x00, bytes([mask & 0x3F]))


def trace_pc(comparator: int, pc: int) -> bytes:
    """Data trace PC value packet: discriminator 0b01cc0."""
    return hw(0x08 | (comparator << 1), struct.pack("<I", pc))


def trace_address(comparator: int, addr16: int) -> bytes:
    """Data trace address packet: discriminator 0b01cc1, 2-byte payload."""
    return hw(0x09 | (comparator << 1), struct.pack("<H", addr16))


def trace_data(comparator: int, write: bool, value: int, size: int = 4) -> bytes:
    """Data trace data value packet: discriminator 0b10ccW."""
    disc = 0x10 | (comparator << 1) | (1 if write else 0)
    return hw(disc, value.to_bytes(size, "little"))


# ---------------------------------------------------------------------------
# TI Log_* framing (LogSinkITM.c)
# ---------------------------------------------------------------------------


def log_record(addr: int, args: Iterable[int] = ()) -> bytes:
    """Log_printf record: header word then one STIM_TRACE word per argument."""
    out = bytearray(sw(STIM_HEADER, struct.pack("<I", addr)))
    for arg in args:
        out += sw(STIM_TRACE, struct.pack("<I", arg & 0xFFFFFFFF))
    return bytes(out)


def log_buf(addr: int, payload: bytes) -> bytes:
    """Log_buf record: header word, length word, then the buffer bytes.

    Mirrors LogSinkITM_bufSingleton + ITM_sendBufferAtomic: 4-byte stimulus
    writes while possible, then 2- and 1-byte writes for the tail.
    """
    out = bytearray(sw(STIM_HEADER, struct.pack("<I", addr)))
    out += sw(STIM_TRACE, struct.pack("<I", len(payload)))
    view = memoryview(bytes(payload))
    while len(view) >= 4:
        out += sw(STIM_TRACE, bytes(view[:4]))
        view = view[4:]
    if len(view) >= 2:
        out += sw(STIM_TRACE, bytes(view[:2]))
        view = view[2:]
    if len(view):
        out += sw(STIM_TRACE, bytes(view[:1]))
    return bytes(out)


def info_timing(prescaler_code: int) -> bytes:
    """STIM_INFO start-of frame: opcode Info_Timing (3) + prescaler code."""
    return sw(STIM_INFO, bytes([3, prescaler_code & 0xFF]))


def info_ts_format(fmt_word: int) -> bytes:
    """STIM_INFO continuation carrying the TimestampP native format word."""
    return sw(STIM_INFO, struct.pack("<I", fmt_word))


def ts_format_word(frac_bytes: int, int_bytes: int, exponent: int, multiplier: int) -> int:
    """Pack a TimestampP_Format word (see ti/drivers/dpl/TimestampP.h)."""
    return (
        (frac_bytes & 0xF)
        | ((int_bytes & 0xF) << 4)
        | ((exponent & 0xFF) << 8)
        | ((multiplier & 0xFFFF) << 16)
    )


def time_sync(native64: int) -> bytes:
    """Two STIM_SYNC_TIME words carrying a 64-bit native timestamp, LSW first
    (LogSinkITM_sendTimeSync)."""
    return sw(STIM_SYNC_TIME, struct.pack("<I", native64 & 0xFFFFFFFF)) + sw(
        STIM_SYNC_TIME, struct.pack("<I", native64 >> 32)
    )


def sink_boot(prescaler_code: int = 2, fmt_word: int | None = None, native64: int = 0) -> bytes:
    """The exact packet sequence LogSinkITM_init puts on the wire:
    reset frame, timing info + native timestamp format, initial time sync."""
    if fmt_word is None:
        # LPF3 FreeRTOS nativeFormat64: 8 integer octets, tick = 8 us
        fmt_word = ts_format_word(0, 8, 6, 8)
    return RESET + info_timing(prescaler_code) + info_ts_format(fmt_word) + time_sync(native64)


# ---------------------------------------------------------------------------
# Benchmark / stress stream profiles
# ---------------------------------------------------------------------------


def dense_block(record_count: int, addr: int, nargs: int = 2, ts_every: int = 8) -> bytes:
    """Back-to-back Log_printf records, an LTS1 every ts_every records.

    Worst case for the packetiser: every frame belongs to a record under
    reassembly. Frame-aligned, so benchmarks can repeat the block endlessly
    after one sink_boot() prefix without growing memory.
    """
    out = bytearray()
    for i in range(record_count):
        if i % ts_every == 0:
            out += lts(1000)
        out += log_record(addr, [i & 0xFFFFFFFF] * nargs)
    return bytes(out)


def dense_log_stream(record_count: int, addr: int, nargs: int = 2, ts_every: int = 8) -> bytes:
    return sink_boot() + dense_block(record_count, addr, nargs, ts_every)


def mixed_block(group_count: int, addr: int) -> bytes:
    """Heavy DWT traffic (PC samples, exceptions, watchpoints, counter wraps)
    interleaved with Log_printf records. Worst case for per-frame dispatch."""
    out = bytearray()
    for i in range(group_count):
        out += lts(200)
        out += pc_sample(0x1000_0000 + ((i * 4) & 0xFFFF))
        out += pc_sample(0x1000_2000 + ((i * 8) & 0xFFFF))
        out += exception(16 + (i % 32), EXC_ENTER)
        out += pc_sleep()
        out += exception(16 + (i % 32), EXC_RETURN)
        out += trace_data(1, True, i & 0xFFFFFFFF)
        if i % 16 == 0:
            out += counter_wrap(0x01)
        out += log_record(addr, [i & 0xFFFFFFFF])
    return bytes(out)


def mixed_hw_stream(group_count: int, addr: int) -> bytes:
    return sink_boot() + mixed_block(group_count, addr)


def stream_checksum(chunks: Iterable[bytes]) -> int:
    """Rolling CRC32 over an iterable of byte chunks (benchmark invariant)."""
    crc = 0
    for chunk in chunks:
        crc = zlib.crc32(chunk, crc)
    return crc
