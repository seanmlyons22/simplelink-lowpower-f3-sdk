"""Offline fixtures for the UART transport tests: a synthetic TraceDB and a
framer+packetiser driver, so every test runs on constructed bytes with no .out
file, mirroring the ITM test setup."""

from __future__ import annotations

import logging
import struct
from typing import List

import pytest

from tilogger.interface import LogPacket
from tilogger.tracedb import ElfString, LOG_ID_MASK

from tilogger_uart_transport.uart_framer import UARTFramer
from tilogger_uart_transport.uart_to_log import UARTPacketiser

# The framer/packetiser log recoverable conditions (resync, unknown id) at
# WARNING/ERROR; tests construct those on purpose, so keep the output quiet.
logging.disable(logging.CRITICAL)


def ts_format_word(frac_bytes: int, int_bytes: int, exponent: int, multiplier: int) -> int:
    """Pack a TimestampP_Format word (see ti/drivers/dpl/TimestampP.h)."""
    return (
        (frac_bytes & 0xF)
        | ((int_bytes & 0xF) << 4)
        | ((exponent & 0xFF) << 8)
        | ((multiplier & 0xFFFF) << 16)
    )


class FakeTraceDB:
    """Duck-typed stand-in for tilogger.tracedb.TraceDB.

    The UART framer/packetiser read .logIndexDB (16-bit log id -> ElfString) and
    .timestamp_fmt_32. add_fmt/add_buf take a .log_ptr slot address and the id is
    its low bits, mirroring the real DB.
    """

    def __init__(self):
        self.traceDB = {}
        self.logIndexDB = {}
        self.elves: List = []
        # Native format: 4 integer octets, x1, exp 0, so a raw timestamp reads
        # back as whole seconds and tests can pin it exactly.
        self.timestamp_fmt_32 = struct.pack("<I", ts_format_word(0, 4, 0, 1))
        self.timestamp_fmt_64 = b""

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


class ListSink(list):
    """Queue stand-in for UARTFramer output: frames land in-order in a list."""

    put = list.append


class FakeLogger:
    def get_system_time(self, device_time: float) -> float:
        return device_time


def packetise(stream: bytes, db: FakeTraceDB) -> List[LogPacket]:
    """Full framer + packetiser pass over a byte stream."""
    sink = ListSink()
    UARTFramer(sink, db).parse(bytearray(stream))
    packetiser = UARTPacketiser(db, FakeLogger())
    packets = []
    for frame in sink:
        packet = packetiser.parse(frame)
        if packet is not None:
            packets.append(packet)
    return packets


@pytest.fixture
def db() -> FakeTraceDB:
    tdb = FakeTraceDB()
    tdb.add_fmt(0x9400_0108, "Count %d of %d", 2)
    tdb.add_fmt(0x9400_0208, "Boot complete", 0, level="Log_WARNING", line="77")
    tdb.add_buf(0x9400_0308, "buffer: ")
    return tdb
