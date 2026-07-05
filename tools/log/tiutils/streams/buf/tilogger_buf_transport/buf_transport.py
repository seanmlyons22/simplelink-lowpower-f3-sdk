"""
Copyright (C) 2026, Texas Instruments Incorporated

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

"""LogSinkBuf transport: poll the sink's RAM ring over SWD and decode it.

The target side (LogSinkBuf.c) writes COBS-framed records into a circular
byte buffer and only ever advances a monotonic write offset; it is built to
run with no host attached. This transport reads the instance struct and the
fresh ring bytes each poll, hands them to the pure decode core
(tilogger.bufdecode), and reconstructs for every record the exact data bytes
the ITM/UART sinks would have sent, so the shared Logger formatter renders
LogSinkBuf logs identically to every other sink.
"""

import struct
import threading
import time
import traceback
from collections import namedtuple
from pathlib import Path
from typing import List, NoReturn, Optional

import typer

from tilogger.bufdecode import decode_buffer
from tilogger.interface import LogPacket, LoggerCliCtx, TransportABC
from tilogger.tracedb import Opcode, TraceDB

from .memory import DumpReader, MemoryReader, PyocdReader

DEFAULT_INSTANCE = "CONFIG_ti_log_LogSinkBuf_0"

# ponytail: hardcoded struct offsets. Both target cores (CM0+, CM33) are
# 32-bit little-endian ARM with the same alignment rules, so the
# LogSinkBuf_Instance layout is fixed: 6 uint32 fields, one uint8, 3 pad.
_STRUCT_FMT = "<6IB3x"
_STRUCT_SIZE = struct.calcsize(_STRUCT_FMT)
assert _STRUCT_SIZE == 28

_Instance = namedtuple("_Instance", ["buffer", "size", "wr_reserve", "last_ts", "rec_count", "overflow", "buf_type"])


def _ticks_factor(fmt_word: bytes) -> float:
    """Ticks-to-seconds factor from the TimestampP native format word.

    LogSinkBuf stamps records with TimestampP_getNative32(), the same source
    the ITM time-sync path announces; the linker copies its format word into
    the ELF as TimestampP_nativeFormat32_copy and TraceDB extracts it. The
    math mirrors TimestampInfo in the itm transport. Without the symbol we
    fall back to reporting raw ticks, like itm/uart do before a time sync.
    """
    if not fmt_word or len(fmt_word) != 4:
        return 1.0
    (value,) = struct.unpack("<I", fmt_word)
    frac_bits = (value & 0xF) * 8
    exponent = (value >> 8) & 0xFF
    multiplier = value >> 16
    if multiplier & 0x8000:
        multiplier -= 0x10000  # signed int16 on the wire
    if multiplier >= 0:
        return (multiplier or 1) * 10.0**-exponent / (1 << frac_bits)
    # negative multiplier: one time unit is abs(multiplier) ticks
    return 10.0**-exponent / (-multiplier * (1 << frac_bits))


class Buf_Transport(TransportABC):
    """Poll a LogSinkBuf instance and feed its records to the Logger.

    reader:   MemoryReader backend (live pyOCD probe or RAM dump).
    trace_db: symbol database built from the --elf files.
    instance: sink instance NAME; the struct symbol is LogSinkBuf_<NAME>_config.
    one_shot: decode a single snapshot then return (dump replay).
    """

    def __init__(
        self,
        reader: MemoryReader,
        trace_db: TraceDB,
        alias: str,
        instance: str = DEFAULT_INSTANCE,
        poll: float = 0.01,
        one_shot: bool = False,
    ):
        super().__init__()
        self._reader = reader
        self._trace_db = trace_db
        self._alias = alias
        self._instance = instance
        self._poll = poll
        self._one_shot = one_shot
        self._ts_factor = _ticks_factor(getattr(trace_db, "timestamp_fmt_32", b""))
        self.stop_event = threading.Event()

        self._struct_addr: Optional[int] = None
        self._buf_addr = 0
        self._size = 0
        self._shadow = bytearray()

        # Decode state carried across polls; see reset().
        self._rd: Optional[int] = None
        self._decoded_prior = 0
        self._dropped = 0
        self._torn = 0
        self._prev_wr: Optional[int] = None
        self._prev_rec: Optional[int] = None
        self._warned_all_torn = False
        self._tally_printed = (0, 0)
        self._tally_time = 0.0

    @property
    def alias(self) -> str:
        return self._alias

    def reset(self):
        """Forget all decode state after a device reboot; the next poll
        re-attaches and decodes the new image's newest window from scratch."""
        self._rd = None
        self._decoded_prior = 0
        self._dropped = 0
        self._torn = 0
        self._prev_wr = None
        self._prev_rec = None
        self._warned_all_torn = False

    def stop(self):
        self.stop_event.set()
        self._reader.close()

    def timestamp_to_seconds(self, timestamp: int) -> float:
        return timestamp * self._ts_factor

    # ------------------------------------------------------------------
    # Target access
    # ------------------------------------------------------------------
    def _attach(self) -> _Instance:
        """Resolve the instance struct, read it once, sanity-check it."""
        if self._struct_addr is None:
            sym = "LogSinkBuf_%s_config" % self._instance
            addr = self._trace_db.symbol_address(sym)
            if addr is None:
                raise RuntimeError(
                    "symbol %s not found in the ELF; wrong --elf file or wrong "
                    "--instance name?" % sym
                )
            self._struct_addr = addr

        inst = self._read_struct()

        if inst.buf_type not in (1, 2):
            raise RuntimeError(
                "LogSinkBuf_%s_config at 0x%08x has bufType=%d (expected 1 or 2); "
                "the ELF does not match the running image" % (self._instance, self._struct_addr, inst.buf_type)
            )
        if not 0 < inst.size <= 1 << 24:
            raise RuntimeError("implausible buffer size %d; ELF/image mismatch" % inst.size)
        # The buffer symbol length is a free cross-check that we are pointed
        # at the right image (local symbols usually survive into .symtab).
        sym_size = self._trace_db.symbol_size("logSinkBuf_%s_buffer" % self._instance)
        if sym_size not in (None, 0, inst.size):
            raise RuntimeError(
                "struct says size=%d but the buffer symbol is %d bytes; "
                "the ELF does not match the running image" % (inst.size, sym_size)
            )

        self._buf_addr = inst.buffer
        self._size = inst.size
        self._shadow = bytearray(inst.size)
        return inst

    def _read_struct(self) -> _Instance:
        return _Instance(*struct.unpack(_STRUCT_FMT, self._reader.read(self._struct_addr, _STRUCT_SIZE)))

    def _read_span(self, start: int, end: int):
        """Pull the ring bytes for monotonic offsets [start, end) into the
        host shadow copy. At most two block reads (one when the span does not
        cross the ring end); never the whole ring once streaming."""
        size = self._size
        if end - start >= size:
            self._shadow[:] = self._reader.read(self._buf_addr, size)
            return
        pos = start % size
        n = end - start
        first = min(n, size - pos)
        self._shadow[pos : pos + first] = self._reader.read(self._buf_addr + pos, first)
        if n > first:
            self._shadow[0 : n - first] = self._reader.read(self._buf_addr, n - first)

    # ------------------------------------------------------------------
    # Decode and emit
    # ------------------------------------------------------------------
    def _poll_once(self, logger):
        inst = self._read_struct()

        # A backward jump in either monotonic counter means the device
        # rebooted (a mere lap is forward and handled inside decode_buffer).
        if self._prev_wr is not None and (inst.wr_reserve < self._prev_wr or inst.rec_count < self._prev_rec):
            typer.secho("[%s] device reboot detected; resetting" % self._alias, fg=typer.colors.YELLOW, err=True)
            self.reset()
            inst = self._attach()
        self._prev_wr, self._prev_rec = inst.wr_reserve, inst.rec_count

        if inst.wr_reserve == (self._rd if self._rd is not None else 0):
            return  # nothing written yet / idle since last poll

        if self._rd is None:
            # Fresh attach (or post-reboot): only the newest window can still
            # be intact, so pull the whole ring once and let the decoder
            # realign. Streaming polls below only ever read the fresh span.
            self._shadow[:] = self._reader.read(self._buf_addr, self._size)
        else:
            self._read_span(self._rd, inst.wr_reserve)

        # decoded_prior is really "records already accounted for": drops
        # counted on an earlier poll must be included, or the next lap counts
        # them again and decoded + dropped overshoots recCount.
        res = decode_buffer(
            self._shadow,
            inst.wr_reserve,
            inst.last_ts,
            inst.rec_count,
            self._trace_db.logIndexDB,
            self._rd,
            self._decoded_prior + self._dropped,
        )
        for rec in res.records:
            self._emit(logger, rec)
        self._rd = res.rd
        self._decoded_prior += res.decoded
        self._dropped += res.dropped
        self._torn += res.torn
        self._report()

    def _emit(self, logger, rec):
        # Rebuild the exact data bytes the ITM/UART sinks put on the wire:
        # 16-bit id, then either raw buffer bytes or 4-byte LE int32 args.
        # Logger.format_dobby_packet then renders LogSinkBuf records
        # byte-identically to every other sink.
        if rec.elf.opcode == Opcode.BUFFER:
            data = struct.pack("<H", rec.log_id) + bytes(rec.args)
        else:
            data = struct.pack("<H", rec.log_id) + b"".join(struct.pack("<I", a & 0xFFFFFFFF) for a in rec.args)

        dev_s = self.timestamp_to_seconds(rec.timestamp)
        if logger is None:
            # Viewer mode (python -m tilogger_buf_transport): no pipeline.
            print("%14.6f  %s" % (dev_s, rec.text))
            return
        host_s = logger.get_system_time(dev_s)
        packet = LogPacket.from_elf_string(rec.elf, data, self._alias, timestamp=host_s, timestamp_local=dev_s)
        logger.log(packet)

    def _report(self):
        if self._decoded_prior == 0 and self._torn > 0 and not self._warned_all_torn:
            self._warned_all_torn = True
            typer.secho(
                "[%s] every frame so far is torn; likely a wrong ELF or wrong --instance" % self._alias,
                fg=typer.colors.RED,
                err=True,
            )
        now = time.monotonic()
        tally = (self._dropped, self._torn)
        if tally != self._tally_printed and (now - self._tally_time >= 1.0 or self._one_shot):
            self._tally_printed = tally
            self._tally_time = now
            typer.secho("[%s] drops=%d torn=%d" % (self._alias, self._dropped, self._torn), err=True)

    # ------------------------------------------------------------------
    # Thread entry
    # ------------------------------------------------------------------
    def start(self, logger) -> NoReturn:
        # Transport threads are daemons: an unhandled exception (or
        # SystemExit) would kill the thread without a word, so report before
        # dying.
        try:
            self._run(logger)
        except Exception:
            traceback.print_exc()
            typer.secho("[%s] buf transport stopped" % self._alias, fg=typer.colors.RED, err=True)

    def _run(self, logger):
        self._attach()
        backoff = self._poll
        while not self.stop_event.is_set():
            try:
                self._poll_once(logger)
                backoff = self._poll
            except Exception as exc:
                if not self._reader.retryable:
                    raise
                # Probe glitch or target reset mid-read: drop the session so
                # the next read reconnects, and back off instead of dying.
                typer.secho(
                    "[%s] probe read failed (%s); retrying" % (self._alias, exc), fg=typer.colors.YELLOW, err=True
                )
                self._reader.close()
                backoff = min(backoff * 2, 5.0)
            if self._one_shot:
                # A dump is a static snapshot: one pass, then done. The
                # pipeline (like from-replayfile) stays up until Ctrl-C.
                typer.secho(
                    "[%s] dump drained: %d records, drops=%d torn=%d"
                    % (self._alias, self._decoded_prior, self._dropped, self._torn),
                    err=True,
                )
                break
            self.stop_event.wait(backoff)


# ----------------------------------------------------------------------
# CLI plumbing (tilogger.transport entry point)
# ----------------------------------------------------------------------
def transport_factory_cli(app: typer.Typer):
    @app.command(name="buf")
    def transport_factory_cb(
        ctx: typer.Context,
        elf: List[Path] = typer.Option([], help="Symbol file path (elf/out file)"),
        instance: str = typer.Option(
            DEFAULT_INSTANCE, help="LogSinkBuf instance NAME; reads the LogSinkBuf_<NAME>_config struct"
        ),
        probe: Optional[str] = typer.Option(None, help="pyOCD probe unique id (default: first probe found)"),
        target: str = typer.Option(
            "cortex_m", help="pyOCD target type; the generic cortex_m target reads CC23xx/CC27xx RAM fine"
        ),
        dump: Optional[Path] = typer.Option(
            None, exists=True, help="Decode a saved RAM dump (.bin) once instead of attaching a probe"
        ),
        base: str = typer.Option("0x20000000", help="Load address of --dump"),
        poll: float = typer.Option(0.01, help="Poll interval in seconds"),
        alias: Optional[str] = typer.Option(None, help="Alias for this device in the log"),
    ):
        """Add LogSinkBuf transport as input to log.

        Reads the LogSinkBuf circular RAM buffer of a running (or halted)
        device over SWD via pyOCD (install with the probe extra:
        pip install -e streams/buf[probe]), or decodes a saved RAM dump with
        --dump. The host only ever reads; the target is never halted and
        never written to.

        You also need to specify .out/.elf files that contain symbol
        information needed to parse the log, but this may also be provided
        globally before adding transports.
        """
        state = ctx.ensure_object(LoggerCliCtx)
        elves = state.symbol_files + elf

        if len(elves) == 0:
            typer.secho(
                "Need elf symbols. Specify via --elf <file> on command line.", fg=typer.colors.BRIGHT_RED, err=True
            )
            raise SystemExit(1)

        db = TraceDB(elves, repickle=False)
        if dump is not None:
            reader: MemoryReader = DumpReader(dump, int(base, 0))
            one_shot = True
        else:
            reader = PyocdReader(probe=probe, target=target)
            one_shot = False
        return Buf_Transport(reader, db, alias or "buf", instance=instance, poll=poll, one_shot=one_shot)
