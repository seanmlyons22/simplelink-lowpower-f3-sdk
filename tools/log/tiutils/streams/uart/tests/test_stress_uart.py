"""Stress and robustness tests for the UART framer: it must never raise on
arbitrary wire data, must recover from corruption via one-byte resync, and
must not let a garbage length field stall the stream."""

import os
import random
import struct
import time

import synth
from conftest import ListSink, packetise

from tilogger_uart_transport.uart_framer import (
    UART_CODE_BUFFER,
    UART_CODE_OVERFLOW,
    UART_FRAME_SYNC,
    UART_MAX_ARGS,
    UARTFramer,
    UARTOpcode,
)


# ---------------------------------------------------------------------------
# Never crash on arbitrary input
# ---------------------------------------------------------------------------
def test_fuzz_random_bytes_never_crash(db):
    """Arbitrary bytes - pure noise or a valid record followed by garbage -
    must parse without ever raising, return promptly, and never hand back
    more leftover than was fed in. This is the load-bearing guarantee: a
    garbled serial line cannot take the host down or grow memory."""
    rng = random.Random(1234)
    start = time.monotonic()
    for _ in range(500):
        n = rng.randint(0, 512)
        blob = bytes(rng.getrandbits(8) for _ in range(n))
        if rng.random() < 0.5:
            # Start aligned on a real record so the fuzz also exercises the
            # consume path, not just the resync path.
            blob = synth.log_printf(0x0108, [1, 2], timestamp=1) + blob
        leftover = UARTFramer(ListSink(), db).parse(bytearray(blob))
        assert len(leftover) <= len(blob)
    # Sanity bound: 500 short streams must not take anywhere near this long;
    # a livelock in the framer loop shows up here instead of hanging CI.
    assert time.monotonic() - start < 30


def test_fuzz_urandom_smoke(db):
    """A few kilobytes of true entropy in one shot: no crash, no growth."""
    for _ in range(4):
        blob = os.urandom(4096)
        leftover = UARTFramer(ListSink(), db).parse(bytearray(blob))
        assert len(leftover) <= len(blob)


# ---------------------------------------------------------------------------
# Corruption recovery: a torn record does not desync the whole stream
# ---------------------------------------------------------------------------
def test_corrupt_middle_record_resyncs(db):
    """A single flipped byte in one record must cost at most that record:
    the framer walks byte-by-byte through the wreckage and the next intact
    record still decodes."""
    rec1 = synth.log_printf(0x0108, [1, 2], timestamp=1)
    rec2 = bytearray(synth.log_printf(0x0208, timestamp=2))
    rec2[1] ^= 0xFF  # corrupt the low id byte: 0x0208 becomes an unknown id
    rec3 = synth.log_printf(0x0108, [3, 4], timestamp=3)
    packets = packetise(rec1 + bytes(rec2) + rec3, db)
    assert [p.timestamp for p in packets] == [1, 3]
    assert packets[1].data == struct.pack("<HII", 0x0108, 3, 4)


def test_truncated_final_record_held_as_leftover(db):
    """A record cut off mid-stream must not emit a packet or crash; the
    partial bytes come back untouched as leftover so the next read can
    complete them."""
    full = synth.log_printf(0x0108, [7, 9], timestamp=5)
    sink = ListSink()
    leftover = UARTFramer(sink, db).parse(bytearray(full[:-3]))
    assert bytes(leftover) == full[:-3]
    # Only the timestamp-format preamble frame, no data frame for the stub.
    assert [f.opcode for f in sink] == [UARTOpcode.TIMESTAMP_FORMAT]


def test_unknown_id_pops_one_byte_not_whole_record(db):
    """An unknown log id means the sync byte was a false lock, so the framer
    must advance exactly one byte - skipping a whole fake record length would
    swallow a real record hiding inside the false one's claimed extent."""
    # False header claims 2 args (15-byte record); a real 7-byte record sits
    # where the fake args would be. A record-sized skip would destroy it.
    fake = bytes([UART_FRAME_SYNC | 2]) + struct.pack("<H", 0xBEEF) + struct.pack("<I", 0x11111111)
    good = synth.log_printf(0x0208, timestamp=2)
    packets = packetise(fake + good + b"\x00", db)
    assert [p.timestamp for p in packets] == [2]


def test_reserved_code_treated_as_false_sync(db):
    """Header codes above UART_MAX_ARGS that are not overflow/buffer are not
    produced by any sink, so they must be treated as line noise: pop one byte
    and keep going, leaving the following record intact."""
    for code in range(UART_MAX_ARGS + 1, UART_CODE_OVERFLOW):
        stream = bytes([UART_FRAME_SYNC | code]) + synth.log_printf(0x0108, [1, 2], timestamp=1)
        packets = packetise(stream, db)
        assert [p.timestamp for p in packets] == [1], f"code 0x{code:X} broke resync"


def test_overflow_record_surfaced_as_frame(db):
    """An overflow record is the sink telling us data was lost; it must reach
    the output as an error frame, not vanish in the framer."""
    sink = ListSink()
    UARTFramer(sink, db).parse(bytearray(synth.overflow(0x0208)))
    errors = [f for f in sink if f.opcode == UARTOpcode.ERROR]
    assert len(errors) == 1
    assert bytes(errors[0].data) == struct.pack("<H", 0x0208)


def test_noise_between_records_all_decode(db):
    """Bursts of random garbage between records model a shared or glitchy
    line; every intact record around the noise must still decode."""
    rng = random.Random(42)
    stream = bytearray()
    for ts in range(1, 9):
        stream += bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 32)))
        stream += synth.log_printf(0x0108, [ts, ts + 1], timestamp=ts)
    packets = packetise(bytes(stream), db)
    assert [p.timestamp for p in packets] == list(range(1, 9))


def test_bogus_buffer_size_field_does_not_stall_stream(db):
    """A noise byte that fakes a buffer-record header carries a garbage
    32-bit size field. The framer must not trust that size before checking
    the log id, or it will sit waiting forever for gigabytes that never
    arrive while real records pile up unread behind the fake header."""
    fake = (
        bytes([UART_FRAME_SYNC | UART_CODE_BUFFER])
        + struct.pack("<H", 0xBEEF)  # id not in the database
        + struct.pack("<I", 0x11111111)  # junk timestamp
        + struct.pack("<I", 0x7FFFFFFF)  # absurd payload size
    )
    good = b"".join(synth.log_printf(0x0108, [ts, ts], timestamp=ts) for ts in range(1, 4))
    packets = packetise(fake + good, db)
    assert [p.timestamp for p in packets] == [1, 2, 3]


def test_known_id_bogus_buffer_size_resyncs(db):
    """Even a buffer header with a real id (0x0308 is a buffer def in the db)
    must not be trusted to name a gigabyte payload: a bit-flip in the size
    field would otherwise stall the stream forever. The payload cap catches it
    and the framer resyncs to the records that follow."""
    fake = (
        bytes([UART_FRAME_SYNC | UART_CODE_BUFFER])
        + struct.pack("<H", 0x0308)  # id IS in the database
        + struct.pack("<I", 0x22222222)  # junk timestamp
        + struct.pack("<I", 0x7FFFFFFF)  # corrupt payload size, past the cap
    )
    good = b"".join(synth.log_printf(0x0108, [ts, ts], timestamp=ts) for ts in range(1, 4))
    packets = packetise(fake + good, db)
    assert [p.timestamp for p in packets] == [1, 2, 3]
