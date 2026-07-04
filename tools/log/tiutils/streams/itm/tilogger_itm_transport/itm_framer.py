"""
Copyright (C) 2020-2024, Texas Instruments Incorporated

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

"""ITM/SWO byte-stream deframer.

Packet encodings follow the ARMv7-M ARM (DDI 0403, Appendix D4) and the
ARMv8-M ARM (DDI 0553); the encodings are identical for every packet the TI
CM3/CM4/CM33 sinks emit. The stream is raw ITM: the TI ITM driver disables
the TPIU formatter (ITM_initHw writes FFCR = 0), so there is no CoreSight
formatter framing to undo.

This module is the receive hot path. It is sized for a sustained 24 MHz SWO
line (~3 MB/s of bytes, order 1e6 frames/s), which is why frames use
__slots__ classes with lazy __str__ instead of dataclasses with eager
f-strings, and why parse() walks the buffer with an index instead of
buf.pop(0)/re-slicing (both O(n) per frame). Measured numbers are in
streams/itm/ARCHITECTURE.md.
"""

import logging
from enum import Enum
from typing import ClassVar, Optional

logger = logging.getLogger("ITM Framer")

HDR_OVERFLOW = 0x70
MAX_ITM_FRAME_SIZE = 5
ITM_RESET_TOKEN = bytes([0xFB, 0xBB, 0xBB, 0xBB, 0xBB])


class ITMStimulusPort(Enum):
    """ITM ports for software packets"""

    STIM_RAW0 = 0
    STIM_RAW1 = 1
    STIM_RAW2 = 2
    STIM_RAW3 = 3
    STIM_RAW4 = 4
    STIM_RAW5 = 5
    STIM_RAW6 = 6
    STIM_RAW7 = 7
    STIM_RAW8 = 8
    STIM_RAW9 = 9
    STIM_RAW10 = 10
    STIM_RAW11 = 11
    STIM_RAW12 = 12
    STIM_RAW13 = 13
    STIM_RAW14 = 14
    STIM_RAW15 = 15
    STIM_RESV16 = 16
    STIM_RESV17 = 17
    STIM_RESV18 = 18
    STIM_RESV19 = 19
    STIM_RESV20 = 20
    STIM_RESV21 = 21
    STIM_RESV22 = 22
    STIM_RESV23 = 23
    STIM_RESV24 = 24
    STIM_RESV25 = 25
    STIM_RESV26 = 26
    STIM_RESV27 = 27
    STIM_TRACE = 28
    STIM_HEADER = 29
    STIM_SYNC_TIME = 30
    STIM_INFO = 31
    STIM_HW = -1


# Port lookup by header bits [7:3]; indexing a list beats Enum.__call__ in the
# per-frame path.
_STIM_PORTS = [ITMStimulusPort(i) for i in range(32)]


class ITMOpcode(Enum):
    """Opcodes for ITM frames that are built in ITMFramer"""

    SYNCHRONIZATION = 0
    EXTENSION = 1
    OVERFLOW = 2
    TIMESTAMP = 3
    PACKET_PC = 4
    SOURCE_SW = 5
    COUNTER_WRAP = 6
    EXCEPTION = 7
    TRACE = 8
    GLOBAL_TIMESTAMP = 9
    UNPARSED = None


HW_EXC_FUNC_DICT = {
    1: "Entered exception indicated by ExceptionNumber field",
    2: "Exited exception indicated by ExceptionNumber field.",
    3: "Returned to exception indicated by ExceptionNumber field.",
}

COUNTER_DICT = {5: "CPI", 4: "Exc", 3: "Sleep", 2: "LSU", 1: "Fold", 0: "Cyc"}

ACCESS_DICT = {
    5: "at {}, Write Access, comparator: {}, value : 0x{:X} ",
    4: "at {}, Read Access, comparator: {}, value : 0x{:X} ",
    3: "at {}, Address access, comparator: {}, value : 0x{:X} ",
    2: "at {}, PC value Access, comparator: {}, value : 0x{:X} ",
}

################################################################################
################################################################################


class ITMFrame:
    """Base ITM frame; concrete frame types subclass this.

    Frames are plain __slots__ classes: the framer creates one object per
    ITM packet (up to ~1e6/s at 24 MHz), so dataclass init machinery and
    eagerly built strings are measurable costs. __str__ is built on demand;
    it is only consumed by the raw viewer and debug logging.
    """

    __slots__ = ("header", "ts_counter", "size")
    opcode: ClassVar[ITMOpcode] = ITMOpcode.UNPARSED

    def __init__(self, header: int, ts_counter: float = 0, size: int = 0):
        self.header = header
        self.ts_counter = ts_counter
        self.size = size

    def __len__(self) -> int:
        return self.size

    def __str__(self) -> str:
        return f"Unparsed ITM frame, header 0x{self.header:02X}"


class ITMSyncFrame(ITMFrame):
    """Synchronization packet: a run of zero bytes ended by 0x80
    (DDI 0403 D4.2.2)."""

    __slots__ = ()
    opcode = ITMOpcode.SYNCHRONIZATION

    def __str__(self) -> str:
        return f"Synchronization packet of size {self.size}"


class ITMExtensionFrame(ITMFrame):
    """Extension packet (DDI 0403 D4.2.6). The single-byte form carries the
    stimulus port page in header bits [6:4]; multi-byte forms continue while
    bit 7 of each payload byte is set."""

    __slots__ = ("value",)
    opcode = ITMOpcode.EXTENSION

    def __init__(self, header: int, value: int, size: int, ts_counter: float = 0):
        super().__init__(header, ts_counter, size)
        self.value = value

    def __str__(self) -> str:
        return f"Extension packet of size {self.size}"


class ITMOverflowFrame(ITMFrame):
    """Overflow packet: single byte 0x70; the ITM dropped at least one packet."""

    __slots__ = ()
    opcode = ITMOpcode.OVERFLOW

    def __str__(self) -> str:
        return "ITM Frame overflow packet"


class ITMTimestampFrame(ITMFrame):
    """Local timestamp packet, both formats (DDI 0403 D4.2.4).

    Format 1: header 0b1TTT0000 plus 1-4 continuation payload bytes.
    Format 2: single byte 0b0TTT0000 carrying a delta of 1..6 directly; the
    delta is in ts_counter and size is 0 (no payload).
    """

    __slots__ = ()
    opcode = ITMOpcode.TIMESTAMP

    timestamp_codes = {0xC: "in sync", 0xD: "TS delayed", 0xE: "packet delayed", 0xF: "packet and timestamp delayed"}

    def __str__(self) -> str:
        if self.header & 0x80:
            relation = ITMTimestampFrame.timestamp_codes.get(self.header >> 4, "RESERVED")
        else:
            # Format 2 exists only when the timestamp is synchronous to data.
            relation = "in sync"
        return f"TIMESTAMP {relation}: + {self.ts_counter} cycles"


class ITMGlobalTimestampFrame(ITMFrame):
    """Global timestamp packet GTS1 (0x94) or GTS2 (0xB4), DDI 0403 D4.2.5.

    The TI sinks never set GTSENA, so these only appear if someone else
    configured the ITM; they are decoded to keep the stream in sync.
    """

    __slots__ = ("value",)
    opcode = ITMOpcode.GLOBAL_TIMESTAMP

    def __init__(self, header: int, value: int, size: int, ts_counter: float = 0):
        super().__init__(header, ts_counter, size)
        self.value = value

    def __str__(self) -> str:
        which = 1 if self.header == 0x94 else 2
        return f"GLOBAL TIMESTAMP {which}: value {self.value}"


class ITMSourceHwPcFrame(ITMFrame):
    """Periodic PC sample packet (DWT discriminator 2): 4-byte PC, or a
    single zero byte meaning the core was sleeping."""

    __slots__ = ("value",)
    opcode = ITMOpcode.PACKET_PC

    def __init__(self, header: int, value: int, size: int, ts_counter: float):
        # Inlined base init: a super().__init__ call per frame is measurable
        # at PC-sampling rates.
        self.header = header
        self.ts_counter = ts_counter
        self.size = size
        self.value = value

    def __str__(self) -> str:
        if self.size == 4:
            return f"Received a PC sample @ {self.ts_counter} PC: 0x{self.value:X}"
        return f"Received a IDLE PC sample @ {self.ts_counter}"


class ITMSourceHwCntWrapFrame(ITMFrame):
    """Event counter wrap packet (DWT discriminator 0): payload bit N set
    means counter N wrapped (see COUNTER_DICT)."""

    __slots__ = ("value",)
    opcode = ITMOpcode.COUNTER_WRAP

    def __init__(self, header: int, value: int, ts_counter: float):
        self.header = header
        self.ts_counter = ts_counter
        self.size = 1
        self.value = value

    def __str__(self) -> str:
        string = "At timestamp {}, the following counter(s) wrapped: ".format(self.ts_counter)
        return string + "".join([COUNTER_DICT[i] + " " if self.value & (1 << i) else "" for i in COUNTER_DICT])


class ITMSourceHwExceptionFrame(ITMFrame):
    """Exception trace packet (DWT discriminator 1): 2-byte payload with a
    9-bit exception number and a 2-bit function (enter/exit/return).

    The 9-bit number field and function encoding are the same in ARMv7-M
    (DDI 0403 D4.3.2) and ARMv8-M (DDI 0553)."""

    __slots__ = ("num_exception", "func_exception")
    opcode = ITMOpcode.EXCEPTION

    def __init__(self, header: int, b0: int, b1: int, ts_counter: float):
        self.header = header
        self.ts_counter = ts_counter
        self.size = 2
        self.num_exception = b0 + ((b1 & 0x1) << 8)
        self.func_exception = (b1 & 0x30) >> 4

    def __str__(self) -> str:
        func = HW_EXC_FUNC_DICT.get(self.func_exception, "Reserved function code")
        return "An Exception has occurred @ {}, Exception Number: {}, Function done: {}".format(
            self.ts_counter, self.num_exception, func
        )


class ITMSourceHwTraceFrame(ITMFrame):
    """Data trace packet (DWT discriminators 8..23): PC value, address, or
    data value read/write for a comparator match (DDI 0403 D4.3.4)."""

    __slots__ = ("hw_packet_type", "data_trace_packet_type", "comparator", "direction", "access_type", "value")
    opcode = ITMOpcode.TRACE

    def __init__(self, header: int, hw_packet_type: int, value: int, size: int, ts_counter: float):
        self.header = header
        self.ts_counter = ts_counter
        self.size = size
        self.hw_packet_type = hw_packet_type
        # Discriminator layout: bits [4:3] = packet type (01 = PC/address,
        # 10 = data value), bits [2:1] = comparator, bit 0 = address/direction.
        self.data_trace_packet_type = hw_packet_type >> 3
        self.comparator = (hw_packet_type >> 1) & 0x3
        self.direction = hw_packet_type & 0x1
        self.access_type = self.direction + (self.data_trace_packet_type << 1)
        self.value = value

    def __str__(self) -> str:
        return "HW Trace " + ACCESS_DICT[self.access_type].format(self.ts_counter / 1000, self.comparator, self.value)


class ITMSourceSWFrame(ITMFrame):
    """Software source (stimulus port) packet: 1, 2 or 4 payload bytes."""

    __slots__ = ("port", "data")
    opcode = ITMOpcode.SOURCE_SW

    def __init__(self, header: int, data: bytearray, ts_counter: float):
        self.header = header
        self.ts_counter = ts_counter
        self.size = len(data)
        self.port = _STIM_PORTS[header >> 3]
        self.data = data

    def __str__(self) -> str:
        return "SW SWIT at +{}, port {}: {}".format(
            self.ts_counter, self.port.name, " ".join((("0x{:02X}".format(i)) for i in self.data))
        )


################################################################################
################################################################################


class ITMFramer:
    """
    Parses serial data into ITMFrames and puts them on the output queue q.

    Args:
        q: Output queue; anything with a put(frame) method.
    """

    def __init__(self, output_queue):
        self._output_queue = output_queue
        self.last_ts_counter = 0
        self._first_read = True

    def parse(self, buf: bytearray) -> bytearray:
        """
        Parse an input buffer into ITMFrames until fewer than
        MAX_ITM_FRAME_SIZE bytes remain (a partial frame or a partial reset
        token may still be in flight, so the tail is carried to the next call).

        Parsing does not start until the reset token (ITM_RESET_TOKEN) has
        been seen; if the token appears inside a later buffer, everything
        before it is discarded and parsing restarts at the token. This is how
        a device reset mid-capture resynchronizes the host.

        Args:
          buf: input buffer; consumed in place.

        Returns:
            The unparsed remainder of buf.
        """
        if not buf:
            return buf

        token_at = buf.find(ITM_RESET_TOKEN)
        if token_at >= 0:
            if token_at:
                # Discard anything before the reset token
                del buf[:token_at]
        # A partial reset token may straddle this read; wait for more data
        # rather than misparsing the token head as frames.
        elif (self._first_read and buf[-1] == 0xBB) or buf[-1] == 0xFB:
            return buf
        elif self._first_read:
            logger.debug("Waiting for a reset frame to begin parsing.")
            return bytearray()

        put = self._output_queue.put
        # One check per buffer, not per frame: logging's isEnabledFor is
        # measurable at frame rates even when the level is off.
        debug = logger.isEnabledFor(logging.DEBUG)
        last_ts = self.last_ts_counter
        # Locals for the loop: global + attribute lookups cost real time at
        # ~1e6 frames/s.
        sw_frame = ITMSourceSWFrame
        pc_frame = ITMSourceHwPcFrame
        exc_frame = ITMSourceHwExceptionFrame
        wrap_frame = ITMSourceHwCntWrapFrame
        trace_frame = ITMSourceHwTraceFrame
        ts_frame = ITMTimestampFrame
        from_bytes = int.from_bytes
        pos = 0
        end = len(buf)
        if end >= MAX_ITM_FRAME_SIZE:
            self._first_read = False

        while end - pos >= MAX_ITM_FRAME_SIZE:
            header = buf[pos]
            pos += 1
            frame: Optional[ITMFrame] = None
            low2 = header & 0x03

            if low2:
                # Source packet: payload is 1, 2 or 4 bytes (size code 0b11).
                # The loop guard proves header + 4 bytes are available.
                size = 4 if low2 == 3 else low2
                payload_end = pos + size
                if header & 0x04 == 0:
                    frame = sw_frame(header, buf[pos:payload_end], last_ts)
                else:
                    disc = header >> 3
                    if disc == 0x02:
                        frame = pc_frame(header, from_bytes(buf[pos:payload_end], "little"), size, last_ts)
                    elif disc == 0x01:
                        if size == 2:
                            frame = exc_frame(header, buf[pos], buf[pos + 1], last_ts)
                        else:
                            logger.error("Exception trace packet with bad size %d", size)
                    elif disc == 0x00:
                        frame = wrap_frame(header, buf[pos], last_ts)
                    elif 0x08 <= disc <= 0x17:
                        frame = trace_frame(header, disc, from_bytes(buf[pos:payload_end], "little"), size, last_ts)
                    else:
                        # Discriminators 3..7 and 24..31 are reserved in both
                        # v7-M and v8-M; skip the encoded payload so the
                        # stream stays in sync.
                        logger.error("Reserved ITM hardware source packet 0x%02X", header)
                pos = payload_end

            elif header == 0x00:
                # Synchronization: consume zeros up to the 0x80 terminator.
                p = pos
                while p < end and buf[p] == 0:
                    p += 1
                if p >= end:
                    # Terminator not received yet; retry when more data lands.
                    pos -= 1
                    break
                if buf[p] == 0x80:
                    p += 1
                frame = ITMSyncFrame(0, last_ts, p - pos)
                pos = p

            elif header == HDR_OVERFLOW:
                logger.warning("ITM Frame Overflow")
                frame = ITMOverflowFrame(header, last_ts)

            elif (header & 0x0F) == 0x00:
                # Local timestamp packet
                if header & 0x80:
                    ts = 0
                    shift = 0
                    p = pos
                    while True:
                        b = buf[p]
                        p += 1
                        ts += (b & 0x7F) << shift
                        shift += 7
                        # Spec caps the payload at 4 bytes; stop there even if
                        # a corrupt byte keeps the continuation bit set.
                        if not b & 0x80 or shift >= 28:
                            break
                    frame = ts_frame(header, ts, p - pos)
                    pos = p
                else:
                    # Format 2: delta 1..6 lives in header bits [6:4].
                    frame = ts_frame(header, (header >> 4) & 0x7, 0)
                last_ts = self.last_ts_counter = frame.ts_counter

            elif (header & 0x0B) == 0x08:
                # Extension packet; single byte unless bit 7 continues it.
                if header & 0x80 == 0:
                    frame = ITMExtensionFrame(header, (header >> 4) & 0x7, 0, last_ts)
                else:
                    value = (header >> 4) & 0x7
                    shift = 3
                    p = pos
                    while p < end:
                        b = buf[p]
                        p += 1
                        value += (b & 0x7F) << shift
                        shift += 7
                        if not b & 0x80 or shift >= 31:
                            break
                    else:
                        pos -= 1
                        break
                    frame = ITMExtensionFrame(header, value, p - pos, last_ts)
                    pos = p

            elif header in (0x94, 0xB4):
                # Global timestamp GTS1/GTS2. GTS2 in 64-bit mode is up to 6
                # payload bytes, which can exceed the loop guard: defer then.
                value = 0
                shift = 0
                p = pos
                incomplete = False
                while True:
                    if p >= end:
                        incomplete = True
                        break
                    b = buf[p]
                    p += 1
                    value += (b & 0x7F) << shift
                    shift += 7
                    if not b & 0x80 or shift >= 49:
                        break
                if incomplete:
                    pos -= 1
                    break
                frame = ITMGlobalTimestampFrame(header, value, p - pos, last_ts)
                pos = p

            else:
                # Unknown header; length is unknowable, resync on next byte.
                logger.error("Invalid ITM header 0x%02X", header)

            if frame is not None:
                if debug:
                    logger.debug("%s", frame)
                put(frame)

        del buf[:pos]
        return buf
