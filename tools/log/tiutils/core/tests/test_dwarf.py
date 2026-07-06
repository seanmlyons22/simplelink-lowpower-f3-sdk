"""DWARF helper coverage, driven by the host-built fixture ELF's debug info.

These functions back PC symbolication and variable/type inspection. None of
them looks at the target architecture - they walk standard DWARF - so the host
fixture exercises the same code a real device .out would.
"""

import io
from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile

from tilogger import dwarf as D


@pytest.fixture
def dwarfinfo(log_elf):
    elf = ELFFile(io.BytesIO(Path(log_elf).read_bytes()))
    assert elf.has_dwarf_info()
    return elf.get_dwarf_info()


def test_get_all_functions(dwarfinfo):
    names = set(D.get_all_functions(dwarfinfo).values())
    assert {"demo_add", "demo_mul", "main"} <= names


def test_make_die_dict_find_symbol_and_name(dwarfinfo):
    dies = D.make_die_dict(dwarfinfo)
    die = D.find_die_for_symbol(dies, "demo_add")
    assert die is not None
    assert D.die_get_name(die) == "demo_add"
    assert D.find_die_for_symbol(dies, "not_here") is None


def test_die_get_file_line(dwarfinfo):
    dies = D.make_die_dict(dwarfinfo)
    die = D.find_die_for_symbol(dies, "demo_add")
    symbol, file, line = D.die_get_file_line(die)
    assert symbol == "demo_add"
    assert file.endswith("funcs.c")
    assert line >= 1


def test_die_get_location_and_location_lookup(dwarfinfo):
    dies = D.make_die_dict(dwarfinfo)
    die = D.find_die_for_symbol(dies, "demo_add")
    loc = D.die_get_location(die)  # a subprogram: DW_AT_low_pc
    assert loc and loc > 0

    locs = D.make_die_location_dict(dies)
    assert locs  # at least the functions have locations
    found = D.get_die_for_location(locs, locs[0][0])
    assert found is not None


def test_recurse_type_follows_return_type(dwarfinfo):
    dies = D.make_die_dict(dwarfinfo)
    die = D.find_die_for_symbol(dies, "demo_add")
    node = D.recurse_type(dies, die)
    assert node.die is die
    # demo_add returns int -> the chain has a next node (the type DIE).
    assert node.next is not None


def test_die_get_subrange_on_non_array_is_none(dwarfinfo):
    dies = D.make_die_dict(dwarfinfo)
    die = D.find_die_for_symbol(dies, "demo_add")
    assert D.die_get_subrange(die) is None


def test_function_range_miss_returns_none(dwarfinfo):
    rd = D.get_all_functions_range([dwarfinfo])
    assert rd.get(0xF000_0000) is None  # PC in no function
