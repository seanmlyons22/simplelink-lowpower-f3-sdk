"""Shared fixtures: a synthetic TraceDB (no .out file needed) and framer/
packetiser drivers, so every test runs offline on constructed bytes."""

from __future__ import annotations

import logging
import shutil
import subprocess
from typing import List

import pytest

from tilogger.dwarf import RangeDict
from tilogger.interface import LogPacket
from tilogger.tracedb import ElfString, LOG_ID_MASK

from tilogger_itm_transport.itm_framer import ITMFramer, ITMFrame
from tilogger_itm_transport.itm_to_log import ITMPacketiser

# The framer/packetiser log recoverable conditions at ERROR; tests construct
# those conditions on purpose, so keep the test output quiet.
logging.disable(logging.CRITICAL)


class FakeTraceDB:
    """Duck-typed stand-in for tilogger.tracedb.TraceDB.

    The packetiser reads .logIndexDB (16-bit log id -> ElfString); building it
    here keeps the Log_* tests offline and fast instead of committing a
    multi-megabyte .out. add_fmt/add_buf take a .log_ptr slot address and the
    id is its low bits, mirroring the real DB.
    """

    def __init__(self):
        self.traceDB = {}
        self.logIndexDB = {}
        self.elves: List = []
        self.timestamp_fmt_32 = b""
        self.timestamp_fmt_64 = b""
        # Tests can inject a RangeDict here to fake DWARF symbolization.
        self.func_ranges = RangeDict({})

    def function_ranges(self) -> RangeDict:
        return self.func_ranges

    def add_fmt(
        self,
        addr: int,
        fmt: str,
        nargs: int,
        level: str = "Log_INFO",
        module: str = "LogMod_App",
        file: str = "app.c",
        line: str = "42",
    ) -> int:
        value = "\x1e".join(["LOG_OPCODE_FORMATED_TEXT", file, line, level, module, fmt, str(nargs)])
        elf_string = ElfString(value, self)
        self.traceDB[addr] = elf_string
        self.logIndexDB[addr & LOG_ID_MASK] = elf_string
        return addr

    def add_buf(
        self,
        addr: int,
        text: str = "buffer: ",
        level: str = "Log_INFO",
        module: str = "LogMod_App",
        file: str = "app.c",
        line: str = "43",
    ) -> int:
        value = "\x1e".join(["LOG_OPCODE_BUFFER", file, line, level, module, text, "0"])
        elf_string = ElfString(value, self)
        self.traceDB[addr] = elf_string
        self.logIndexDB[addr & LOG_ID_MASK] = elf_string
        return addr


class FakeLogger:
    """Stand-in for tilogger.logger.Logger: only get_system_time is used by
    the packetiser (resync frames)."""

    def __init__(self):
        self.timebase = None

    def get_system_time(self, device_time: float) -> float:
        # Deterministic: host time == device time, so global timestamps in
        # tests equal local ones and can be pinned exactly.
        return device_time


class ListSink(list):
    """Queue stand-in for ITMFramer output: frames land in-order in a list.

    put aliases list.append directly so the framer's per-frame put() call
    does not pay an extra Python frame (this sink is also the benchmark's).
    """

    put = list.append


def drain(stream: bytes) -> List[ITMFrame]:
    """Feed one byte stream through a fresh framer, return the parsed frames."""
    sink = ListSink()
    ITMFramer(sink).parse(bytearray(stream))
    return list(sink)


def drain_chunked(stream: bytes, chunk_size: int) -> List[ITMFrame]:
    """Same, but delivered in chunk_size pieces with remainder carry, the way
    the serial loop delivers data."""
    sink = ListSink()
    framer = ITMFramer(sink)
    buf = bytearray()
    for off in range(0, len(stream), chunk_size):
        buf.extend(stream[off : off + chunk_size])
        buf = framer.parse(buf)
    return list(sink)


def packetise(stream: bytes, db: FakeTraceDB, **kwargs) -> List[LogPacket]:
    """Full framer + packetiser pass over a byte stream."""
    packetiser = ITMPacketiser(db, FakeLogger(), **kwargs)
    packets = []
    for frame in drain(stream):
        packet = packetiser.parse(frame)
        if packet is not None:
            packets.append(packet)
    return packets


@pytest.fixture
def db() -> FakeTraceDB:
    tdb = FakeTraceDB()
    tdb.add_fmt(0x9000_0100, "Count %d of %d", 2)
    tdb.add_fmt(0x9000_0200, "Boot complete", 0, level="Log_WARNING", line="77")
    tdb.add_buf(0x9000_0300, "buffer: ")
    return tdb


# A tiny host-arch ELF with .log_data/.log_ptr sections, so the CLI callback can
# build a real TraceDB without a device .out. Architecture-neutral: TraceDB reads
# sections/symbols, never the machine type. Skips where no cc is available.
_LOGSEC_S = r"""
	.section .log_data,"a",@progbits
LogSymbol_x:
	.asciz "LOG_OPCODE_FORMATED_TEXT\036file.c\03642\036Log_DEBUG\036LogMod_App\036x=%d\0361"
	.size LogSymbol_x, .-LogSymbol_x
	.section .log_ptr,"a",@progbits
Ptr_LogSymbol_x:
	.long LogSymbol_x
	.size Ptr_LogSymbol_x, 4
"""


@pytest.fixture(scope="session")
def log_elf(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C toolchain to build the log ELF fixture")
    d = tmp_path_factory.mktemp("logelf")
    (d / "logsec.s").write_text(_LOGSEC_S)
    elf = d / "fixture.elf"
    try:
        subprocess.run([cc, "-c", str(d / "logsec.s"), "-o", str(d / "logsec.o")],
                       check=True, capture_output=True)
        subprocess.run([cc, "-nostdlib", "-no-pie", "-Wl,--entry=0", str(d / "logsec.o"), "-o", str(elf)],
                       check=True, capture_output=True)
    except (subprocess.CalledProcessError, OSError) as exc:
        pytest.skip("log ELF fixture build failed: %s" % exc)
    return elf
