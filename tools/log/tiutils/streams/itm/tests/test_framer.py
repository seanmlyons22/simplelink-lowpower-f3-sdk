"""Deframer tests on constructed bytes: every frame type, split buffers,
reset-token handling, and the raw-viewer strings pinned byte-for-byte."""

import pytest

import synth
from conftest import ListSink, drain, drain_chunked

from tilogger_itm_transport.itm_framer import (
    ITMFramer,
    ITMOpcode,
    ITMStimulusPort,
)


def mixed_stream() -> bytes:
    stream = bytearray(synth.RESET)
    stream += synth.lts(1000)
    stream += synth.sw(0, b"A")
    stream += synth.pc_sample(0xDEADBEEF)
    stream += synth.exception(15, synth.EXC_ENTER)
    stream += synth.trace_data(1, True, 0x1234)
    stream += synth.counter_wrap(0x20)
    stream += synth.lts(50)
    stream += b"\xff" * 5  # flush tail: framer holds back the last <5 bytes
    return bytes(stream)


def test_every_frame_type_roundtrips():
    frames = drain(mixed_stream())
    opcodes = [f.opcode for f in frames]
    assert opcodes[:8] == [
        ITMOpcode.SOURCE_SW,  # the reset token itself is a port-31 SW frame
        ITMOpcode.TIMESTAMP,
        ITMOpcode.SOURCE_SW,
        ITMOpcode.PACKET_PC,
        ITMOpcode.EXCEPTION,
        ITMOpcode.TRACE,
        ITMOpcode.COUNTER_WRAP,
        ITMOpcode.TIMESTAMP,
    ]


def test_frame_fields():
    frames = drain(mixed_stream())
    reset, ts1, sw0, pc, exc, trc, wrap, ts2 = frames[:8]

    assert reset.port == ITMStimulusPort.STIM_INFO
    assert bytes(reset.data) == b"\xbb\xbb\xbb\xbb"

    assert ts1.ts_counter == 1000
    assert ts2.ts_counter == 50

    assert sw0.port == ITMStimulusPort.STIM_RAW0
    assert bytes(sw0.data) == b"A"
    assert sw0.ts_counter == 1000  # stamped with the preceding LTS delta

    assert pc.value == 0xDEADBEEF
    assert pc.size == 4

    assert exc.num_exception == 15
    assert exc.func_exception == synth.EXC_ENTER

    assert trc.comparator == 1
    assert trc.value == 0x1234
    assert trc.access_type == 5  # write

    assert wrap.value == 0x20  # CPI wrapped


def test_raw_viewer_strings_pinned():
    """str(frame) is the raw-viewer (logger=None) output; pinned exactly."""
    frames = drain(mixed_stream())
    assert [str(f) for f in frames[:8]] == [
        "SW SWIT at +0, port STIM_INFO: 0xBB 0xBB 0xBB 0xBB",
        "TIMESTAMP in sync: + 1000 cycles",
        "SW SWIT at +1000, port STIM_RAW0: 0x41",
        "Received a PC sample @ 1000 PC: 0xDEADBEEF",
        "An Exception has occurred @ 1000, Exception Number: 15, Function done: "
        "Entered exception indicated by ExceptionNumber field",
        "HW Trace at 1.0, Write Access, comparator: 1, value : 0x1234 ",
        "At timestamp 1000, the following counter(s) wrapped: CPI ",
        "TIMESTAMP in sync: + 50 cycles",
    ]


@pytest.mark.parametrize("chunk_size", [1, 2, 3, 4, 5, 7, 11, 64])
def test_split_across_reads_is_lossless(chunk_size):
    """Frames split at any buffer boundary decode identically, including the
    reset token split across two reads."""
    whole = [(f.opcode, str(f)) for f in drain(mixed_stream())]
    chunked = [(f.opcode, str(f)) for f in drain_chunked(mixed_stream(), chunk_size)]
    assert chunked == whole


def test_nothing_parses_before_reset_token():
    sink = ListSink()
    framer = ITMFramer(sink)
    leftover = framer.parse(bytearray(synth.lts(1000) + synth.pc_sample(4) + b"\xff" * 5))
    assert sink == []
    assert leftover == bytearray()


def test_garbage_before_reset_token_is_discarded():
    stream = b"\x13\x37\xff\xee" + mixed_stream()
    frames = drain(stream)
    assert frames[0].opcode == ITMOpcode.SOURCE_SW
    assert bytes(frames[0].data) == b"\xbb\xbb\xbb\xbb"
    assert len(frames) == len(drain(mixed_stream()))


def test_reset_token_in_later_read_resyncs():
    """A reset token arriving in a later read discards the garbage before it.
    (The token scan runs once per parse call, on the buffer as received.)"""
    sink = ListSink()
    framer = ITMFramer(sink)
    framer.parse(bytearray(mixed_stream()))
    first_count = len(sink)
    framer.parse(bytearray(b"\x99\x88\x77\x66\x55\x44" + mixed_stream()))
    assert len(sink) == 2 * first_count
    # The first frame of the second half is the reset token itself.
    assert bytes(sink[first_count].data) == b"\xbb\xbb\xbb\xbb"


def test_short_tail_is_carried_not_dropped():
    """Anything shorter than the max frame size stays buffered for next read."""
    sink = ListSink()
    framer = ITMFramer(sink)
    stream = synth.RESET + synth.pc_sample(0x1234)
    # Deliver all but the last byte of the PC sample frame.
    leftover = framer.parse(bytearray(stream[:-1]))
    assert [f.opcode for f in sink] == [ITMOpcode.SOURCE_SW]
    leftover.extend(stream[-1:] + b"\xff" * 5)
    framer.parse(leftover)
    assert [f.opcode for f in sink][:2] == [ITMOpcode.SOURCE_SW, ITMOpcode.PACKET_PC]
    assert sink[1].value == 0x1234


def test_overflow_frame_is_surfaced():
    """Overflow packets mean the device dropped data; they are emitted so the
    packetiser can warn the user, and they do not desync the stream."""
    frames = drain(synth.RESET + synth.overflow() + synth.pc_sample(0xABCD) + b"\xff" * 5)
    assert [f.opcode for f in frames][:3] == [ITMOpcode.SOURCE_SW, ITMOpcode.OVERFLOW, ITMOpcode.PACKET_PC]
    assert str(frames[1]) == "ITM Frame overflow packet"
    assert frames[2].value == 0xABCD


def test_sw_frame_sizes():
    """1/2/4-byte stimulus writes all decode with correct payloads."""
    stream = synth.RESET + synth.sw(5, b"\x11") + synth.sw(6, b"\x22\x33") + synth.sw(7, b"\x44\x55\x66\x77")
    frames = drain(stream + b"\xff" * 5)
    sizes = [(f.port.value, bytes(f.data)) for f in frames[1:4]]
    assert sizes == [(5, b"\x11"), (6, b"\x22\x33"), (7, b"\x44\x55\x66\x77")]


# ---------------------------------------------------------------------------
# Spec conformance (ARM DDI 0403 / DDI 0553); byte layouts constructed from
# the documents, not from the decoder, so a wrong assumption fails loudly.
# ---------------------------------------------------------------------------


def test_lts2_single_byte_timestamp():
    """Local timestamp format 2 (0b0TTT0000) carries a delta of 1..6 in the
    header itself. The old parser dropped these as 'reserved'."""
    stream = synth.RESET
    for delta in range(1, 7):
        stream += synth.lts2(delta)
    stream += synth.sw(0, b"Z") + b"\xff" * 5
    frames = drain(stream)
    ts_frames = [f for f in frames if f.opcode == ITMOpcode.TIMESTAMP]
    assert [f.ts_counter for f in ts_frames] == [1, 2, 3, 4, 5, 6]
    assert str(ts_frames[0]) == "TIMESTAMP in sync: + 1 cycles"
    # The SW frame is stamped with the last LTS2 delta
    assert frames[-1].ts_counter == 6


def test_lts1_multibyte_values():
    """LTS1 LEB-style payload: up to 4 bytes, 7 bits each."""
    for cycles in (1, 127, 128, 16383, 16384, 2**27 - 1):
        frames = drain(synth.RESET + synth.lts(cycles) + b"\xff" * 5)
        ts = [f for f in frames if f.opcode == ITMOpcode.TIMESTAMP]
        assert ts[0].ts_counter == cycles, cycles


def test_global_timestamp_packets_keep_sync():
    """GTS1/GTS2 are never sent by the TI sinks (GTSENA stays 0) but must not
    desync the stream if some other agent enabled them."""
    stream = synth.RESET + synth.gts1(0x123456) + synth.gts2(0x1FFF) + synth.pc_sample(0x42) + b"\xff" * 5
    frames = drain(stream)
    gts = [f for f in frames if f.opcode == ITMOpcode.GLOBAL_TIMESTAMP]
    assert [g.value for g in gts] == [0x123456, 0x1FFF]
    assert frames[-1].opcode == ITMOpcode.PACKET_PC
    assert frames[-1].value == 0x42


def test_extension_packet_single_byte():
    """Single-byte extension: page number in bits [6:4], no payload."""
    stream = synth.RESET + synth.extension(3) + synth.sw(1, b"\x55") + b"\xff" * 5
    frames = drain(stream)
    ext = [f for f in frames if f.opcode == ITMOpcode.EXTENSION]
    assert len(ext) == 1
    assert ext[0].value == 3
    assert ext[0].size == 0
    # The byte after the extension is decoded normally
    assert frames[-1].opcode == ITMOpcode.SOURCE_SW
    assert bytes(frames[-1].data) == b"\x55"


def test_sync_packet_spec_terminator():
    """ARM sync packet: >= 47 zero bytes ended by 0x80."""
    stream = synth.RESET + synth.sync_packet(47, terminator=0x80) + synth.pc_sample(0x99) + b"\xff" * 5
    frames = drain(stream)
    assert frames[1].opcode == ITMOpcode.SYNCHRONIZATION
    assert frames[2].opcode == ITMOpcode.PACKET_PC
    assert frames[2].value == 0x99


def test_sync_split_across_reads():
    """Zeros with no terminator yet are held, not consumed as garbage."""
    sink = ListSink()
    framer = ITMFramer(sink)
    leftover = framer.parse(bytearray(synth.RESET + bytes(20)))
    assert [f.opcode for f in sink] == [ITMOpcode.SOURCE_SW]
    leftover.extend(bytes(27) + b"\x80" + synth.pc_sample(0x77) + b"\xff" * 5)
    framer.parse(leftover)
    assert [f.opcode for f in sink] == [ITMOpcode.SOURCE_SW, ITMOpcode.SYNCHRONIZATION, ITMOpcode.PACKET_PC]


def test_reserved_hw_discriminators_skip_payload():
    """Discriminators 3..7 and 24..31 are reserved in v7-M and v8-M; the
    encoded payload length must still be honored so the stream stays in sync."""
    stream = synth.RESET
    stream += synth.hw(0x03, b"\xaa\xbb\xcc\xdd")  # reserved, 4-byte payload
    stream += synth.hw(0x18, b"\x11")  # reserved, 1-byte payload
    stream += synth.pc_sample(0x1234)
    stream += b"\xff" * 5
    frames = drain(stream)
    # No frames for the reserved packets, and the PC sample still decodes.
    opcodes = [f.opcode for f in frames]
    assert ITMOpcode.PACKET_PC in opcodes
    assert frames[-1].value == 0x1234
    assert len(frames) == 2  # reset SW frame + PC sample


def test_exception_field_layout():
    """9-bit exception number and 2-bit function field (DDI 0403 D4.3.2)."""
    # Exception 256 needs the 9th bit; function 'exit' = 2 in bits [5:4].
    frames = drain(synth.RESET + synth.exception(256 + 15, synth.EXC_EXIT) + b"\xff" * 5)
    exc = frames[1]
    assert exc.num_exception == 271
    assert exc.func_exception == 2


def test_data_trace_discriminator_layout():
    """Data trace discriminators (DDI 0403 D4.3.4): type in bits [4:3],
    comparator in [2:1], direction/address bit 0."""
    stream = synth.RESET
    stream += synth.trace_pc(2, 0x08001234)
    stream += synth.trace_address(1, 0x2000)
    stream += synth.trace_data(3, False, 0x55)
    stream += synth.trace_data(0, True, 0xAA55, 2)
    stream += b"\xff" * 5
    frames = drain(stream)
    pc_m, addr_m, rd, wr = frames[1:5]
    assert (pc_m.access_type, pc_m.comparator, pc_m.value) == (2, 2, 0x08001234)
    assert (addr_m.access_type, addr_m.comparator, addr_m.value) == (3, 1, 0x2000)
    assert (rd.access_type, rd.comparator, rd.value) == (4, 3, 0x55)
    assert (wr.access_type, wr.comparator, wr.value) == (5, 0, 0xAA55)


def test_pc_sample_sleep_variant():
    frames = drain(synth.RESET + synth.pc_sleep() + b"\xff" * 5)
    pc = frames[1]
    assert pc.opcode == ITMOpcode.PACKET_PC
    assert pc.size == 1
    assert "IDLE" in str(pc)


def test_mixed_spec_stream_chunk_sweep():
    """Every packet type in one stream, delivered at every chunk size."""
    stream = bytearray(synth.RESET)
    stream += synth.lts(1000) + synth.lts2(3)
    stream += synth.gts1(77) + synth.extension(1)
    stream += synth.sync_packet(47, 0x80)
    stream += synth.pc_sample(0xCAFE) + synth.exception(16, synth.EXC_ENTER)
    stream += synth.trace_data(1, True, 0x77) + synth.counter_wrap(0x3F)
    stream += synth.overflow() + synth.sw(0, b"\x42")
    stream += b"\xff" * 5
    whole = [(f.opcode, str(f)) for f in drain(bytes(stream))]
    for chunk_size in (1, 2, 3, 5, 8, 13, 64):
        assert [(f.opcode, str(f)) for f in drain_chunked(bytes(stream), chunk_size)] == whole, chunk_size
