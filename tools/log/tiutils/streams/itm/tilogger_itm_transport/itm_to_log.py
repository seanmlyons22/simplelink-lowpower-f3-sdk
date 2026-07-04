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

"""ITMFrame -> LogPacket packetiser.

Reassembles TI Log_* records from STIM_HEADER/STIM_TRACE stimulus frames and
surfaces the DWT hardware events (PC samples, exception trace, watchpoints,
counter wraps) plus ITM overflows as LogPackets, so they appear on stdout and
in Wireshark next to the Log_* records with the same clock.

This is on the receive hot path together with itm_framer; debug logging is
gated on a flag captured at construction because logging call overhead is
measurable at frame rates (see streams/itm/ARCHITECTURE.md).
"""

import enum
import logging
import os
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field
from typing import Optional, Tuple

from tilogger.dwarf import RangeDict
from tilogger.interface import LogLevel, LogPacket
from tilogger.logger import Logger
from tilogger.tracedb import ElfString, Opcode

from .itm_framer import ITMFrame, ITMOpcode, ITMSourceSWFrame, ITMStimulusPort

logger = logging.getLogger("ItmPacketiser")

SWIT_SIZE = 4
RESET_TOKEN = bytes([0xBB, 0xBB, 0xBB, 0xBB])

# Custom LogPacket opcodes for surfaced hardware events. Log.h reserves 0-9
# (tilogger.interface); these are ITM-transport specific.
DWT_OPCODE_PC_SAMPLE = 10
DWT_OPCODE_EXCEPTION = 11
DWT_OPCODE_WATCHPOINT = 12
DWT_OPCODE_COUNTER_WRAP = 13
ITM_OPCODE_OVERFLOW = 14

DWT_MODULE = "DWT"
ITM_MODULE = "ITM"

# ARMv7-M / ARMv8-M system exception numbers (B1.5 in both ARMs). 7 is
# SecureFault on v8-M only; on v7-M it is reserved and never traced.
_EXCEPTION_NAMES = {
    0: "Thread",
    1: "Reset",
    2: "NMI",
    3: "HardFault",
    4: "MemManage",
    5: "BusFault",
    6: "UsageFault",
    7: "SecureFault",
    11: "SVCall",
    12: "DebugMonitor",
    14: "PendSV",
    15: "SysTick",
}

_EXCEPTION_FN = {1: "entry", 2: "exit", 3: "return"}

# Data-trace access_type (see ITMSourceHwTraceFrame) -> text
_WATCHPOINT_TEXT = {
    2: "watchpoint {c}: PC match, PC 0x{v:08X}",
    3: "watchpoint {c}: address match, addr 0x{v:X}",
    4: "watchpoint {c}: read, value 0x{v:X}",
    5: "watchpoint {c}: write, value 0x{v:X}",
}

# Counter-wrap bit names (DWT CTRL order)
COUNTER_NAMES = {5: "CPI", 4: "Exc", 3: "Sleep", 2: "LSU", 1: "Fold", 0: "Cyc"}

# Hot-path aliases: parse() runs per frame, and each Enum attribute access
# would be a global load plus an attribute load.
_OP_TIMESTAMP = ITMOpcode.TIMESTAMP
_OP_SOURCE_SW = ITMOpcode.SOURCE_SW
_OP_PACKET_PC = ITMOpcode.PACKET_PC
_OP_EXCEPTION = ITMOpcode.EXCEPTION
_OP_TRACE = ITMOpcode.TRACE
_OP_COUNTER_WRAP = ITMOpcode.COUNTER_WRAP
_OP_OVERFLOW = ITMOpcode.OVERFLOW
_PORT_INFO = ITMStimulusPort.STIM_INFO
_PORT_SYNC_TIME = ITMStimulusPort.STIM_SYNC_TIME
_PORT_HEADER = ITMStimulusPort.STIM_HEADER
_PORT_TRACE = ITMStimulusPort.STIM_TRACE
_LVL_VERBOSE = LogLevel.Log_VERBOSE
_LVL_INFO = LogLevel.Log_INFO
_LVL_WARNING = LogLevel.Log_WARNING

# Rendered counter-wrap texts by payload byte (only 64 possible values).
_WRAP_CACHE: dict = {}


def exception_name(number: int) -> str:
    if number >= 16:
        return f"IRQ{number - 16}"
    return _EXCEPTION_NAMES.get(number, f"Exception{number}")


class InfoOps(enum.Enum):
    TIMESTAMP_INFO = 3


class ItmLogPacketData:
    """One TI Log_* record while it is being reassembled from stimulus frames."""

    def __init__(
        self,
        elf_string: ElfString,
        header: ITMSourceSWFrame,
        alias: str,
        timestamp: Optional[float] = None,
        timestamp_local: Optional[float] = None,
    ):
        self.remaining_length: int
        self.elf_string = elf_string
        self.alias = alias
        self.timestamp = timestamp
        self.timestamp_local = timestamp_local
        self.data: bytes = bytes(header.data)
        self.next_frame_has_length: bool = False

        if self.elf_string.opcode == Opcode.FORMATTED_TEXT:
            self.remaining_length = int(elf_string.nargs) * SWIT_SIZE

        elif self.elf_string.opcode == Opcode.BUFFER:
            # Real length arrives in the next STIM_TRACE frame
            # (LogSinkITM_bufSingleton sends it before the payload).
            self.remaining_length = 1024
            self.next_frame_has_length = True

        else:
            self.remaining_length = 0

    def append(self, itm_frame: ITMSourceSWFrame) -> None:
        if self.next_frame_has_length:
            self.next_frame_has_length = False
            self.remaining_length = int.from_bytes(itm_frame.data, "little")
        else:
            self.remaining_length -= itm_frame.size
            self.data += bytes(itm_frame.data)

    def to_log_packet(self) -> LogPacket:
        return LogPacket.from_elf_string(self.elf_string, self.data, self.alias, self.timestamp, self.timestamp_local)


@dataclass
class TimestampInfo:
    """Converts device-native timestamps (STIM_SYNC_TIME words) to seconds.

    The device announces its format as a TimestampP_Format word
    (ti/drivers/dpl/TimestampP.h):

        uint32_t fracBytes:4;   octets (LSB) of fractional part
        uint32_t intBytes:4;    octets (MSB) of integer part
        uint32_t exponent:8;    decimal scale to seconds
        int32_t  multiplier:16; ticks-to-time factor; negative = divide

    int_width/frac_width here are in bits.
    """

    exponent: int
    multiplier: int
    int_width: int
    frac_width: int
    _curr_word: Optional[int] = field(default=None, repr=False)

    @staticmethod
    def from_native_format(ts_format: bytes) -> "TimestampInfo":
        # "<I": the wire word is little-endian 32-bit. A bare "L" is 8 bytes
        # on LP64 hosts (Linux/macOS) and broke this parse there.
        (value,) = struct.unpack("<I", ts_format)
        # fracBytes/intBytes count octets (TimestampP.h), not bits.
        frac_width = (value & 0xF) * 8
        int_width = ((value >> 4) & 0xF) * 8
        exponent = (value >> 8) & 0xFF
        multiplier = value >> 16
        if multiplier & 0x8000:
            multiplier -= 0x10000  # signed int16 on the wire
        return TimestampInfo(exponent, multiplier, int_width, frac_width)

    def parse_native(self, word: int) -> Optional[float]:
        """Feed one 32-bit word; returns seconds once enough words arrived.

        Formats wider than 32 bits (e.g. the 64-bit LPF3 native format) come
        as two words, LSW first (LogSinkITM_sendTimeSync).
        """
        if self._curr_word is not None:
            word = word << 32 | self._curr_word
            self._curr_word = None
        elif self.int_width + self.frac_width > 32:
            self._curr_word = word
            return None

        fractional = (word & ((1 << self.frac_width) - 1)) / 2**self.frac_width if self.frac_width else 0.0
        integral = (word >> self.frac_width) & ((1 << self.int_width) - 1)
        # multiplier < 0 means one time unit is abs(multiplier) ticks
        # (TimestampP.h); 0 would be a corrupt format word.
        if self.multiplier >= 0:
            scale = self.multiplier or 1
            return scale * (integral + fractional) * 10**-self.exponent
        return (integral + fractional) / -self.multiplier * 10**-self.exponent


class ITMPacketiser:
    """
    Parses ITM frames into LogPackets.

    Args:
        db: trace database (symbols from the --elf files)
        logsink: Logger instance, used for host/device time correlation
        alias: stream name shown in outputs
        clock: CPU clock of the embedded device in Hz (local timestamp base)
        baud: TPIU SWO baud rate, used to spread frame times inside one
              timestamp window by their wire time
    """

    def __init__(self, db=None, logsink: Optional[Logger] = None, alias="ITM0", clock=48000000, baud=12000000):
        self._trace_db = db
        self._info_opcode: Optional[InfoOps] = None
        self._ts_info: Optional[TimestampInfo] = None
        self._rtc_s = 0.0
        self.prescaler = 1
        self.clock = clock
        self.global_timestamp_delta: Optional[float] = None
        self.offset = 0.0
        self.baudrate = int(baud)
        self.logger = logsink
        self.alias = alias

        # PC-sample histogram: (function, file, line) -> count. Enabled by the
        # transport when the user asks for a profile; None keeps the per-frame
        # cost at zero otherwise.
        self.pc_histogram: Optional[Counter] = None

        self._inv_baud = 1.0 / self.baudrate
        self._current_packet: Optional[ItmLogPacketData] = None
        self._func_ranges: Optional[RangeDict] = None
        # Render caches: PC samples and exception events repeat heavily (hot
        # loops, periodic interrupts), and symbolization plus string
        # formatting dominates the per-frame cost otherwise.
        self._pc_cache: dict = {}
        self._exc_cache: dict = {}
        # Captured once: logging call overhead is measurable per-frame.
        self._debug = logger.isEnabledFor(logging.DEBUG)

    def parse(self, itm_frame: ITMFrame) -> Optional[LogPacket]:
        """
        The top-level ITMFrame parser: routes frames by opcode.

        Local timestamp frames advance the running device clock and emit
        nothing. Software source frames feed Log_* record reassembly.
        DWT/overflow frames become LogPackets directly.

        Args:
          itm_frame: input ITMFrame

        Returns:
            A completed LogPacket, or None if this frame did not finish one.
        """
        try:
            # Module-level constants (not Enum attribute lookups): this runs
            # once per frame.
            opcode = itm_frame.opcode
            if opcode is _OP_TIMESTAMP:
                # Timestamps are deltas in prescaled CPU cycles since the
                # previous timestamp packet.
                self._rtc_s += itm_frame.ts_counter / (self.clock / self.prescaler)
                self.offset = 0.0
                return None

            # Spread frames inside a timestamp window by their own wire time:
            # header byte + payload at the SWO baud rate (10 bits per byte on
            # the NRZ line, but /8 keeps the historical calibration).
            self.offset += (itm_frame.size + 1) * self._inv_baud

            if opcode is _OP_SOURCE_SW:
                port = itm_frame.port
                if port is _PORT_INFO:
                    self.parse_control_frame(itm_frame, self.offset)
                elif port is _PORT_SYNC_TIME:
                    self.parse_resync_frame(itm_frame, self.offset)

                packet = self.append_packet(itm_frame, self.offset)
                if packet:
                    return packet.to_log_packet()
                return None

            if opcode is _OP_PACKET_PC:
                return self._pc_packet(itm_frame)
            if opcode is _OP_EXCEPTION:
                return self._exception_packet(itm_frame)
            if opcode is _OP_TRACE:
                return self._watchpoint_packet(itm_frame)
            if opcode is _OP_COUNTER_WRAP:
                return self._counter_wrap_packet(itm_frame)
            if opcode is _OP_OVERFLOW:
                return self._overflow_packet(itm_frame)

        except Exception as exc:  # noqa: BLE001 - one bad frame must not kill the stream
            exc_type, _, exc_tb = sys.exc_info()
            if exc_tb:
                fname = os.path.split(exc_tb.tb_frame.f_code.co_filename)[1]
                logger.error("{} @ {} {}: ".format(exc_type, fname, exc_tb.tb_lineno) + str(exc))

        return None

    # ------------------------------------------------------------------
    # Surfaced hardware events
    # ------------------------------------------------------------------

    def _hw_packet(
        self,
        opcode: int,
        level: LogLevel,
        text: str,
        data: bytes,
        filename: str = "dwt",
        lineno: str = "0",
        module: str = DWT_MODULE,
    ) -> LogPacket:
        ts_local = self._rtc_s + self.offset
        packet = LogPacket(
            self.alias,
            module,
            opcode,
            level,
            filename,
            lineno,
            ts_local + (self.global_timestamp_delta or 0),
            ts_local,
            data,
            self._trace_db,
        )
        # Custom opcodes (>= 10) skip Logger's ELF formatting; the rendered
        # text is provided directly, the same way from-replayfile does it.
        packet._str_data = text
        return packet

    def _pc_packet(self, frame: ITMFrame) -> LogPacket:
        if frame.size != 4:
            # Single-byte variant: the core was asleep at sample time.
            if self.pc_histogram is not None:
                self.pc_histogram[("<sleep>", None, None)] += 1
            return self._hw_packet(DWT_OPCODE_PC_SAMPLE, _LVL_VERBOSE, "pc sample: sleep", b"\x00")

        pc = frame.value
        cached = self._pc_cache.get(pc)
        if cached is None:
            text = f"pc sample: 0x{pc:08X}"
            filename, lineno = "dwt", "0"
            key = (f"0x{pc:08X}", None, None)
            location = self._symbolize(pc)
            if location is not None:
                _, _, func, file, line = location
                text = f"pc sample: 0x{pc:08X} {func} ({file}:{line})"
                filename, lineno = file, str(line)
                key = (func, file, line)
            cached = (text, struct.pack("<I", pc), filename, lineno, key)
            self._pc_cache[pc] = cached
        text, data, filename, lineno, key = cached
        if self.pc_histogram is not None:
            self.pc_histogram[key] += 1
        return self._hw_packet(DWT_OPCODE_PC_SAMPLE, _LVL_VERBOSE, text, data, filename, lineno)

    def _exception_packet(self, frame: ITMFrame) -> LogPacket:
        cache_key = (frame.num_exception, frame.func_exception)
        cached = self._exc_cache.get(cache_key)
        if cached is None:
            fn = _EXCEPTION_FN.get(frame.func_exception, f"fn={frame.func_exception}")
            name = exception_name(frame.num_exception)
            text = f"exception {frame.num_exception} ({name}) {fn}"
            data = struct.pack("<H", frame.num_exception | (frame.func_exception << 12))
            cached = (text, data)
            self._exc_cache[cache_key] = cached
        return self._hw_packet(DWT_OPCODE_EXCEPTION, _LVL_INFO, cached[0], cached[1])

    def _watchpoint_packet(self, frame: ITMFrame) -> LogPacket:
        text = _WATCHPOINT_TEXT[frame.access_type].format(c=frame.comparator, v=frame.value)
        return self._hw_packet(DWT_OPCODE_WATCHPOINT, _LVL_INFO, text, frame.value.to_bytes(4, "little"))

    def _counter_wrap_packet(self, frame: ITMFrame) -> LogPacket:
        cached = _WRAP_CACHE.get(frame.value)
        if cached is None:
            names = [COUNTER_NAMES[i] for i in COUNTER_NAMES if frame.value & (1 << i)]
            cached = ("counter wrap: " + " ".join(names), bytes([frame.value]))
            _WRAP_CACHE[frame.value] = cached
        return self._hw_packet(DWT_OPCODE_COUNTER_WRAP, _LVL_VERBOSE, cached[0], cached[1])

    def _overflow_packet(self, frame: ITMFrame) -> LogPacket:
        text = "ITM overflow: the device dropped at least one trace packet"
        return self._hw_packet(ITM_OPCODE_OVERFLOW, _LVL_WARNING, text, b"", "itm", "0", ITM_MODULE)

    def _symbolize(self, pc: int) -> Optional[Tuple]:
        """PC -> (CU, DIE, function, file, line) via the DWARF info of the
        --elf files; None if unresolved or no DWARF available."""
        if self._func_ranges is None:
            self._func_ranges = self._load_func_ranges()
        return self._func_ranges.get(pc)

    def _load_func_ranges(self) -> RangeDict:
        try:
            if self._trace_db is not None:
                return self._trace_db.function_ranges()
        except Exception as exc:  # noqa: BLE001 - missing DWARF must not stop decoding
            logger.warning("PC symbolization unavailable: %s", exc)
        return RangeDict({})

    # ------------------------------------------------------------------
    # Control / time sync
    # ------------------------------------------------------------------

    def handle_ts_info(self, prescaler=None, ts_format=None):
        if prescaler is not None:
            # ITM TCR.TSPrescale encoding (DDI 0403 C1.7.1); the sink sends
            # the raw field value in the Info_Timing immediate.
            prescaler_lut = {0: 1, 1: 4, 2: 16, 3: 64}
            self.prescaler = prescaler_lut[prescaler]
            logger.debug("Local timestamp prescaler is %d", self.prescaler)

        if ts_format is not None:
            self._ts_info = TimestampInfo.from_native_format(bytes(ts_format))
            logger.debug("Native timestamp format is %s", self._ts_info)
            self._info_opcode = None  # done parsing this opcode

    def parse_control_frame(self, itm_frame: ITMSourceSWFrame, time_offset) -> None:
        # Size 4 is only ever continuation
        if itm_frame.size == 4:
            (word,) = struct.unpack("<I", itm_frame.data)
            if word == 0xBBBBBBBB:
                logger.debug("Parsed reset control frame")
            elif self._info_opcode == InfoOps.TIMESTAMP_INFO:
                self.handle_ts_info(ts_format=itm_frame.data)

        # Size 2 is start-of with immediate
        elif itm_frame.size == 2:
            opcode, imm = struct.unpack("BB", bytes(itm_frame.data))
            self._info_opcode = InfoOps(opcode)
            if self._info_opcode == InfoOps.TIMESTAMP_INFO:
                self.handle_ts_info(prescaler=imm)

        # Size 1 is start-of, possibly indicating 32-bit to follow
        elif itm_frame.size == 1:
            self._info_opcode = InfoOps(itm_frame.data[0])

    def parse_resync_frame(self, itm_frame: ITMSourceSWFrame, time_offset) -> None:
        if self._ts_info is None:
            logger.warning("Time sync frame before timestamp format info; ignored")
            return
        dev_time = self._ts_info.parse_native(struct.unpack("<I", itm_frame.data)[0])
        if dev_time:
            logger.debug("Device clock is %0.5f, overriding current time of %0.5f", dev_time, self._rtc_s)
            self._rtc_s = dev_time

            if self.global_timestamp_delta is None and self.logger is not None:
                self.global_timestamp_delta = self.logger.get_system_time(dev_time) - dev_time

    # ------------------------------------------------------------------
    # Log_* record reassembly
    # ------------------------------------------------------------------

    def append_packet(self, itm_frame: ITMSourceSWFrame, time_offset) -> Optional[ItmLogPacketData]:
        port = itm_frame.port
        if port is _PORT_HEADER:
            header = int.from_bytes(itm_frame.data, "little")

            trace_db = self._trace_db.traceDB
            if header not in trace_db:
                # This address does not exist in the trace database
                logger.warning("FRAMING: corruption: no trace database information at 0x%x", header)
                self._current_packet = None
                return None

            self._current_packet = ItmLogPacketData(
                trace_db[header],
                itm_frame,
                alias=self.alias,
                timestamp_local=self._rtc_s + time_offset,
                timestamp=self._rtc_s + time_offset + (self.global_timestamp_delta or 0),
            )
            if self._debug:
                logger.debug(
                    "FRAMING: New Frame with len %d header 0x%x", self._current_packet.remaining_length, header
                )

            if self._current_packet.remaining_length == 0:
                packet = self._current_packet
                self._current_packet = None
                return packet

        elif port is _PORT_TRACE:
            if not self._current_packet:
                logger.warning("Unexpected trace packet with no header! Discarding.")
            else:
                self._current_packet.append(itm_frame)
                if self._debug:
                    logger.debug(
                        "FRAMING: %d bytes added, remaining: %d", itm_frame.size, self._current_packet.remaining_length
                    )

                # < 0 would mean a trace packet was appended to a header that
                # declared no data; only possible with misconfigured opcodes.
                if self._current_packet.remaining_length <= 0:
                    packet = self._current_packet
                    self._current_packet = None
                    return packet

        return None

    def reset(self):
        """Handle reset frame."""
        pass
