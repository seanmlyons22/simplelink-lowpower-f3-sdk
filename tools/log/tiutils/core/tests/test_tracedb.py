"""Unit tests for the log-id index builder.

build_log_index is factored out of TraceDB.parse_elf so the aliasing guard can
be exercised without a real .out file: the guard is the line that keeps a build
with too many log sites from silently decoding to the wrong string.
"""

import pytest

from tilogger.tracedb import build_log_index, LOG_ID_MASK


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
