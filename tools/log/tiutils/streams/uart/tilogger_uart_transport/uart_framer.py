"""
Copyright (C) 2023-2024, Texas Instruments Incorporated

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the
    distribution.

    Neither the name of Texas Instruments Incorporated nor the names of
    its contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

import logging
from dataclasses import dataclass, field
from enum import Enum

from typing import Optional

from tilogger.tracedb import ElfString, Opcode, TraceDB, LOG_ID_SIZE
from tilogger.helpers import build_value

logger = logging.getLogger("UART Framer")
# To enable debug output, uncomment the following line
# logging.basicConfig(level=logging.DEBUG)

UART_RESET_TOKEN = bytes([0xBB, 0xBB, 0xBB, 0xBB])

# Every record starts with a one-byte frame header: a fixed sync pattern in the
# high nibble and a record code in the low nibble (see LogSinkUART.c). This lets
# the framer resynchronize to the byte stream without a wide address value.
UART_FRAME_SYNC = 0xA0
UART_SYNC_MASK = 0xF0
UART_CODE_MASK = 0x0F
UART_CODE_OVERFLOW = 0x0E
UART_CODE_BUFFER = 0x0F
UART_MAX_ARGS = 8

_FRAME_HEADER_SIZE = 1
_TIMESTAMP_SIZE = 4
_SIZE_FIELD_SIZE = 4
# Frame header byte plus the 16-bit log id.
_ID_HEADER_SIZE = _FRAME_HEADER_SIZE + LOG_ID_SIZE


class UARTOpcode(Enum):
    """Opcodes for UART frames that are built in UARTFramer"""

    DATA = 0
    ERROR = 1
    TIMESTAMP_FORMAT = 2
    UNPARSED = None


################################################################################
################################################################################


@dataclass
class UARTFrame:
    """Base UART frame that stores common information and should be subclassed by other frames"""

    header: int
    opcode: UARTOpcode
    ts_counter: float = 0
    # value: int = 0

    size: int = 0
    string: str = "Frame has not yet been parsed"

    def __len__(self):
        return self.size

    def __str__(self):
        return self.string


@dataclass
class UARTDataFrame(UARTFrame):
    """Log data frame"""

    opcode: UARTOpcode = UARTOpcode.DATA
    data: bytearray = field(default_factory=bytearray)

    def parse(self, buf, size):
        """Build UARTDataFrame from buf"""
        self.size = size
        # Store data
        self.data = buf[: self.size]
        # build string
        self.string = " ".join((("0x{:02X}".format(i)) for i in self.data))
        return buf[self.size :]

    def __str__(self):
        return self.string


@dataclass
class UARTErrorFrame(UARTFrame):
    """Log error frame"""

    opcode: UARTOpcode = UARTOpcode.ERROR
    data: bytearray = field(default_factory=bytearray)

    def parse(self, buf):
        """Build UARTErrorFrame from buf"""
        self.size = 4
        # Store data
        self.data = buf[: self.size]
        # build string
        self.string = " ".join((("0x{:02X}".format(i)) for i in self.data))
        return buf[self.size :]

    def __str__(self):
        return self.string


@dataclass
class UARTTimestampFormatFrame(UARTFrame):
    """Log timestamp format frame"""

    opcode: UARTOpcode = UARTOpcode.TIMESTAMP_FORMAT
    data: bytearray = field(default_factory=bytearray)

    def parse(self, buf):
        """Build UARTTimestampFormatFrame from buf"""
        self.size = 4
        # Store data
        self.data = buf[: self.size]
        # build string
        self.string = " ".join((("0x{:02X}".format(i)) for i in self.data))
        return buf[self.size :]

    def __str__(self):
        return self.string


################################################################################
################################################################################


class UARTFramer:
    """
    Manages parsing serial data into UARTFrames and outputs UARTFrames onto output queue q

    Args:
        q: Output queue

    """

    def __init__(self, output_queue, trace_db: TraceDB):
        # Create the PDU stream thread.
        self._output_queue = output_queue
        self.last_ts_counter = 0
        self._trace_db = trace_db

    def parse(self, buf: bytearray):
        """
        Parse as many bytes as possible from the input buffer. If the buffer contains less than 4 bytes,
        nothing is extracted

        Args:
          buf: input buffer to parse

        Returns:
            Unparsed portion of the input buffer

        """
        if not buf:
            return buf

        if len(self._trace_db.timestamp_fmt_32) == 0:
            raise Exception(f"Timestamp Format not found: {self._trace_db.timestamp_fmt_32}")

        frame: Optional[UARTFrame] = None
        frame = UARTTimestampFormatFrame(0, opcode=UARTOpcode.TIMESTAMP_FORMAT)
        frame.parse(self._trace_db.timestamp_fmt_32)
        self._output_queue.put(frame)

        # While there is a full record to parse...
        while True:
            # Need at least a frame header and a log id to make any decision.
            if len(buf) < _ID_HEADER_SIZE:
                logger.debug("Not enough data to parse.")
                return buf

            # A record starts with the sync pattern; anything else is stream
            # noise, so advance one byte and try again.
            if (buf[0] & UART_SYNC_MASK) != UART_FRAME_SYNC:
                buf.pop(0)
                continue

            code = buf[0] & UART_CODE_MASK

            if code <= UART_MAX_ARGS:
                # printf: header + id + timestamp + one word per argument
                record_length = _ID_HEADER_SIZE + _TIMESTAMP_SIZE + code * 4
                is_buffer = False
            elif code == UART_CODE_OVERFLOW:
                record_length = _ID_HEADER_SIZE
                is_buffer = False
            elif code == UART_CODE_BUFFER:
                # The payload size follows the timestamp; wait for it if needed.
                if len(buf) < _ID_HEADER_SIZE + _TIMESTAMP_SIZE + _SIZE_FIELD_SIZE:
                    return buf
                size_offset = _ID_HEADER_SIZE + _TIMESTAMP_SIZE
                payload_size = build_value(buf[size_offset : size_offset + _SIZE_FIELD_SIZE])
                record_length = _ID_HEADER_SIZE + _TIMESTAMP_SIZE + _SIZE_FIELD_SIZE + payload_size
                is_buffer = True
            else:
                # Reserved code: a false sync, advance one byte.
                buf.pop(0)
                continue

            if len(buf) < record_length:
                return buf

            # Validate the log id against the database. An unknown id means we
            # locked onto a byte that only looked like a frame header, so resync.
            log_id = build_value(buf[_FRAME_HEADER_SIZE : _FRAME_HEADER_SIZE + LOG_ID_SIZE])
            if log_id not in self._trace_db.logIndexDB:
                buf.pop(0)
                continue

            try:
                if code == UART_CODE_OVERFLOW:
                    frame = UARTErrorFrame(0)
                    frame.data = buf[_FRAME_HEADER_SIZE:record_length]
                    frame.size = len(frame.data)
                    self._output_queue.put(frame)
                else:
                    frame = UARTDataFrame(0)
                    if is_buffer:
                        # Drop the frame header and the size field, leaving
                        # [id timestamp payload] for the packetiser to finish.
                        payload_start = _ID_HEADER_SIZE + _TIMESTAMP_SIZE + _SIZE_FIELD_SIZE
                        frame.data = buf[_FRAME_HEADER_SIZE : _ID_HEADER_SIZE + _TIMESTAMP_SIZE] + buf[
                            payload_start:record_length
                        ]
                    else:
                        # Drop the frame header, leaving [id timestamp args].
                        frame.data = buf[_FRAME_HEADER_SIZE:record_length]
                    frame.size = len(frame.data)
                    logger.debug("Parsed data frame (size %d): %s", frame.size, frame)
                    self._output_queue.put(frame)
            except Exception as exc:  # pylint: disable=broad-except
                logger.error("Invalid UART Packet")
                logger.debug(exc)

            buf = buf[record_length:]

        # Return unparsed data
        return buf
