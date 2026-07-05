"""Unit tests for the LogSinkBuf circular-byte-buffer decoder.

These drive the host decode core (tilogger.bufdecode) with synthetic buffers
produced by RingWriter, which mirrors the target enqueue in LogSinkBuf.c
byte-for-byte. They prove the whole scheme end to end without a device: records
round-trip, timestamps reconstruct from the struct anchor, overwritten records
are counted exactly, and torn/incomplete frames are handled.
"""

import pytest

from tilogger.tracedb import ElfString, Opcode
from tilogger.bufdecode import (
    RingWriter,
    decode_buffer,
    cobs_encode,
    cobs_decode,
    uleb_encode,
    uleb_decode,
    TYPE_LINEAR,
)


def make_elf(fmt, nargs, opcode="LOG_OPCODE_FORMATED_TEXT"):
    """Build an ElfString the way TraceDB does from a .log_data record."""
    value = "\x1e".join([opcode, "file.c", "42", "Log_DEBUG", "LogMod_App", fmt, str(nargs)])
    return ElfString(value, None)


# Log-site index keyed the way tracedb.logIndexDB is (slot address & 0xFFFF, so
# the low two bits are zero because .log_ptr slots are 4-byte aligned).
INDEX = {
    0x0008: make_elf("boot", 0),
    0x000C: make_elf("x=%d", 1),
    0x0010: make_elf("a=%d b=%d", 2),
    0x0014: make_elf("%d %d %d %d %d %d %d %d", 8),
    0x0018: make_elf("dump ", 0, opcode="LOG_OPCODE_BUFFER"),
}


# ---------------------------------------------------------------------------
# Primitive round-trips
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("value", [0, 1, 127, 128, 16383, 0xFFFFFFFF])
def test_uleb_roundtrip(value):
    data = uleb_encode(value)
    decoded, pos = uleb_decode(data, 0)
    assert decoded == value
    assert pos == len(data)


@pytest.mark.parametrize(
    "payload",
    [b"", b"\x01", b"\x00", b"\x00\x00\x00", b"ab\x00cd", bytes(range(256)), b"\x00" * 300],
)
def test_cobs_roundtrip_and_no_interior_zero(payload):
    frame = cobs_encode(payload)
    assert frame[-1] == 0  # trailing delimiter
    assert 0 not in frame[:-1]  # COBS guarantees no interior zero
    assert cobs_decode(frame[:-1]) == payload


# ---------------------------------------------------------------------------
# 1. Record round-trip: ids, args, formatted text, and timestamps
# ---------------------------------------------------------------------------
def test_record_roundtrip_args_and_timestamps():
    writer = RingWriter(size=512)
    # (log_id, now, args)
    writes = [
        (0x0008, 10, ()),
        (0x000C, 25, (42,)),
        (0x0010, 25, (1, 2)),  # same tick -> zero delta
        (0x000C, 900, (0,)),  # arg 0
        (0x000C, 5000, (0xFFFFFFFF,)),  # 5-byte ULEB arg
        (0x0014, 70000, (1, 2, 3, 4, 5, 6, 7, 8)),  # 8 args, multi-byte delta
    ]
    for log_id, now, args in writes:
        writer.printf(log_id, now, args)

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    assert res.torn == 0
    assert res.dropped == 0
    assert res.decoded == len(writes)

    # ids and formatted text
    assert [r.log_id for r in res.records] == [w[0] for w in writes]
    assert res.records[0].text == "boot"
    assert res.records[1].text == "x=42"
    assert res.records[2].text == "a=1 b=2"
    assert res.records[3].text == "x=0"
    assert res.records[4].text == "x=4294967295"
    assert res.records[5].text == "1 2 3 4 5 6 7 8"

    # timestamps reconstruct to the absolute values that were logged
    assert [r.timestamp for r in res.records] == [w[1] for w in writes]


def test_buffer_record():
    writer = RingWriter(size=512)
    payload = bytes([0x00, 0x11, 0x00, 0xAB])  # interior zeros exercise COBS
    writer.printf(0x0008, 5)
    writer.log_buf(0x0018, 30, payload)

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    assert res.torn == 0
    assert res.dropped == 0
    assert res.decoded == 2
    buf_rec = res.records[1]
    assert buf_rec.args == list(payload)
    assert buf_rec.text == "dump 0x0 0x11 0x0 0xab"
    assert buf_rec.timestamp == 30


# ---------------------------------------------------------------------------
# 2. Drop accounting: every committed record is decoded or counted as dropped
# ---------------------------------------------------------------------------
def test_drop_accounting_on_overwrite():
    # A small circular buffer forced to wrap several times.
    writer = RingWriter(size=64)
    for i in range(200):
        writer.printf(0x0010, 100 + i, (i, i * 2))

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    # The invariant the user asked for: nothing is silently lost.
    assert res.decoded + res.dropped == writer.rec_count
    # The scenario really did overwrite older records.
    assert res.dropped > 0
    assert res.decoded > 0
    # Whatever survived decodes cleanly and is the newest run of records.
    assert res.torn == 0
    assert res.records[-1].args == [199, 398]


def test_inflight_frontier_is_not_a_drop():
    writer = RingWriter(size=512)
    for i in range(4):
        writer.printf(0x000C, 10 + i, (i,))
    # Simulate a record reserved (recCount bumped) whose body is still being
    # written: advance the frontier over some non-zero bytes with no delimiter.
    frontier = writer.wr_reserve
    for k in range(5):
        writer.buf[(frontier + k) % writer.size] = 0x11
    writer.wr_reserve += 5
    writer.rec_count += 1

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    assert res.decoded == 4
    assert res.dropped == 0  # the in-flight frame is retried, not dropped
    assert res.torn == 0
    assert res.rd == frontier  # read cursor stops at the incomplete frame


# ---------------------------------------------------------------------------
# 3. Tearing: corrupt/unknown frames are counted, good frames still decode
# ---------------------------------------------------------------------------
def test_torn_frame_unknown_id():
    writer = RingWriter(size=512)
    writer.printf(0x000C, 10, (1,))
    writer.printf(0xBEEF, 20, (2,))  # id absent from INDEX -> torn
    writer.printf(0x0010, 30, (3, 4))

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    assert res.torn == 1
    assert res.decoded == 2
    assert [r.log_id for r in res.records] == [0x000C, 0x0010]


def test_corrupt_cobs_is_torn_and_resyncs():
    writer = RingWriter(size=512)
    writer.printf(0x000C, 10, (1,))
    good_end = writer.wr_reserve
    writer.printf(0x0010, 20, (2, 3))
    writer.printf(0x000C, 30, (4,))

    # Corrupt a byte inside the second frame (between the first two delimiters).
    writer.buf[good_end + 1] ^= 0xFF

    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)

    # First and third frames still decode; the mangled one is flagged.
    assert res.torn >= 1
    ids = [r.log_id for r in res.records]
    assert 0x000C in ids
    assert res.records[-1].text == "x=4"  # decoder resynced to the last frame


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
