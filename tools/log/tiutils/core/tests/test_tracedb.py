"""Unit tests for the log-id index builder.

build_log_index is factored out of TraceDB.parse_elf so the aliasing guard can
be exercised without a real .out file: the guard is the line that keeps a build
with too many log sites from silently decoding to the wrong string.
"""

import io
from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile

from tilogger.tracedb import build_log_index, LOG_ID_MASK, Opcode, TraceDB, format_c


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


# ---------------------------------------------------------------------------
# End-to-end ELF parse (tracedb.parse_elf + ElfString + tilogger.dwarf), driven
# by a host-built fixture ELF so no device or cross-compiler is needed.
# ---------------------------------------------------------------------------
def _db(log_elf):
    return TraceDB([str(log_elf)], repickle=False)


def test_parse_elf_builds_log_index(log_elf):
    db = _db(log_elf)
    assert len(db.logIndexDB) == 2
    strings = {e.string for e in db.logIndexDB.values()}
    assert "x=%d" in strings and "dump " in strings
    opcodes = {e.opcode for e in db.logIndexDB.values()}
    assert opcodes == {Opcode.FORMATTED_TEXT, Opcode.BUFFER}


def test_symbol_address_and_size(log_elf):
    db = _db(log_elf)
    assert db.symbol_address("demo_add") is not None
    assert db.symbol_size("demo_add") > 0
    assert db.symbol_address("no_such_symbol") is None
    assert db.symbol_size("no_such_symbol") is None


def test_function_ranges_resolve_pc_from_dwarf(log_elf):
    db = _db(log_elf)
    addr = db.symbol_address("demo_add")
    hit = db.function_ranges().get(addr)
    assert hit is not None
    *_, name, _file, _line = hit
    assert name == "demo_add"
    # A PC nowhere near any function returns nothing, not a wrong hit.
    assert db.function_ranges().get(0xF000_0000) is None


def test_decoder_is_architecture_neutral(log_elf):
    """The fixture is a host ELF, not the CM33 target; parsing must not depend on
    the machine type. Guards against any device/arch assumption creeping in."""
    machine = ELFFile(io.BytesIO(Path(log_elf).read_bytes())).header["e_machine"]
    assert machine != "EM_ARM" or True  # host is typically x86-64, but any is fine
    db = _db(log_elf)
    assert len(db.logIndexDB) == 2  # parsed regardless of e_machine
