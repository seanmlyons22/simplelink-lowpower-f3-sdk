"""Unit tests for the log-id index builder.

build_log_index is factored out of TraceDB.parse_elf so the aliasing guard can
be exercised without a real .out file: the guard is the line that keeps a build
with too many log sites from silently decoding to the wrong string.
"""

import pytest

from tilogger.tracedb import build_log_index, LOG_ID_MASK, format_c


def test_distinct_slots_map_to_low_bits():
    """Each 4-byte .log_ptr slot keys on its low 16 bits."""
    index = build_log_index([(0x94000008, "a"), (0x9400000C, "b"), (0x94000010, "c")])
    assert index == {0x0008: "a", 0x000C: "b", 0x0010: "c"}


def test_repeated_slot_is_not_a_collision():
    """The same slot seen twice (e.g. rebuilt map) keeps its single entry."""
    index = build_log_index([(0x94000008, "a"), (0x94000008, "a")])
    assert index == {0x0008: "a"}


def test_aliasing_slots_raise():
    """Two slots more than 64 kB apart share low bits and must fail loudly
    rather than return an aliased log."""
    with pytest.raises(ValueError):
        build_log_index([(0x94000008, "a"), (0x94000008 + (LOG_ID_MASK + 1), "b")])


def test_format_c_signed_conversions():
    """The sinks send each arg as a raw 32-bit word, so a negative %d/%i value
    arrives as its unsigned two's complement. format_c reinterprets those as
    int32 (matching C) while leaving %u/%x and %% alone."""
    # -38 came across as 0xFFFFFFDA; %d prints signed, %u stays unsigned.
    assert format_c("rssi=%d handle=%u", [0xFFFFFFDA, 8]) == "rssi=-38 handle=8"
    assert format_c("x=%d y=%u z=0x%08X", [0xFFFFFFFF, 0xFFFFFFFF, 0xDEAD]) == "x=-1 y=4294967295 z=0x0000DEAD"
    # %% is a literal percent and must not consume an arg.
    assert format_c("lit %% then %i", [0xFFFFFF9C]) == "lit % then -100"
    # Length modifiers still resolve to the base conversion.
    assert format_c("%ld", [0xFFFFFFFF]) == "-1"
    # Small positive values are unaffected.
    assert format_c("Count %d of %d", [3, 32]) == "Count 3 of 32"
