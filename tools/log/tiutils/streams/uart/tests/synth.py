"""Synthetic UART byte-stream encoder for the LogSinkUART tests.

Written from the wire format in source/ti/log/LogSinkUART.c (frame header byte,
16-bit log id, 32-bit timestamp, then per-record fields), independent of the
decoder so a round-trip through the framer/packetiser is a real conformance
check and not a tautology.
"""

from __future__ import annotations

import struct
from typing import Iterable

from tilogger_uart_transport.uart_framer import UART_FRAME_SYNC, UART_CODE_BUFFER, UART_CODE_OVERFLOW


def log_printf(log_id: int, args: Iterable[int] = (), timestamp: int = 0) -> bytes:
    """printf record: frame header (sync | argc), id, timestamp, then args."""
    args = list(args)
    out = bytearray([UART_FRAME_SYNC | len(args)])
    out += struct.pack("<H", log_id & 0xFFFF)
    out += struct.pack("<I", timestamp & 0xFFFFFFFF)
    for arg in args:
        out += struct.pack("<I", arg & 0xFFFFFFFF)
    return bytes(out)


def log_buf(log_id: int, payload: bytes, timestamp: int = 0) -> bytes:
    """buffer record: frame header (sync | buffer code), id, timestamp, size, payload."""
    out = bytearray([UART_FRAME_SYNC | UART_CODE_BUFFER])
    out += struct.pack("<H", log_id & 0xFFFF)
    out += struct.pack("<I", timestamp & 0xFFFFFFFF)
    out += struct.pack("<I", len(payload))
    out += bytes(payload)
    return bytes(out)


def overflow(log_id: int) -> bytes:
    """overflow record: frame header (sync | overflow code) then id."""
    return bytes([UART_FRAME_SYNC | UART_CODE_OVERFLOW]) + struct.pack("<H", log_id & 0xFFFF)
