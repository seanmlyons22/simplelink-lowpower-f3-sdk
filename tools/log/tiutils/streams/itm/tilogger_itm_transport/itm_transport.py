"""
Copyright (C) 2021-2024, Texas Instruments Incorporated

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

from collections import Counter
from pathlib import Path
import sys
import atexit
import argparse
import threading

from typing import Optional, NoReturn, List

from tilogger.interface import LogPacket, LoggerCliCtx, TransportABC
from tilogger.logger import Logger
from tilogger.tracedb import TraceDB

from .serial_rx import SerialRx
from .itm_framer import ITMFramer
from .itm_to_log import ITMPacketiser
from .pcsample import write_speedscope_profile, open_in_speedscope

import typer


class _FrameSink(list):
    """Framer output: frames appended in order, drained by the same thread.

    The old per-frame queue.Queue paid a lock round trip per frame plus a
    1 ms blocking get per loop turn; the framer and the packetiser run on
    the same thread, so a plain list is enough.
    """

    put = list.append


class ITM_Transport(TransportABC):
    def __init__(self, port: str, baudrate: int, trace_db: TraceDB, alias: str,
                 pcsample: Optional[Path] = None, late_attach: bool = False):
        super().__init__()

        self._com_port = port
        self._baud_rate = baudrate
        self._trace_db = trace_db
        self._alias = alias
        self._pcsample = pcsample
        self._late_attach = late_attach
        self._packetiser: Optional[ITMPacketiser] = None
        self._profile_written = False
        self.serial: Optional[SerialRx] = None
        self.stop_event = threading.Event()

    @property
    def alias(self):
        return self._alias

    def stop(self):
        self.stop_event.set()
        self._write_profile()

    def _write_profile(self):
        """Dump the PC-sample profile and open the viewer, once, on shutdown."""
        if self._profile_written or self._pcsample is None or self._packetiser is None:
            return
        self._profile_written = True
        histogram = self._packetiser.pc_histogram
        if not histogram:
            typer.secho("No DWT PC samples captured; no profile written.", err=True)
            return
        out = write_speedscope_profile(histogram, self._pcsample, name=self.alias)
        typer.secho(f"PC-sample profile written to {out}", err=True)
        open_in_speedscope(out)

    def start(self, logger: Optional[Logger]) -> NoReturn:
        self.serial = SerialRx(self._com_port, self._baud_rate, alias=self._alias)
        atexit.register(self.serial.close)

        frames = _FrameSink()
        framer = ITMFramer(frames, late_attach=self._late_attach)

        # Note we use "Logger == None" as our 'ITM only mode' flag
        if logger:
            packetiser = ITMPacketiser(self._trace_db, logger, self.alias)
            if self._pcsample is not None:
                packetiser.pc_histogram = Counter()
            self._packetiser = packetiser
            # The transport thread is a daemon: it dies without unwinding on
            # exit, so the profile dump must also hang off process exit.
            atexit.register(self._write_profile)
        else:
            packetiser = None

        rx_data: bytearray = bytearray()
        while not self.stop_event.is_set():
            # Block briefly in the reader when the line is idle; when data is
            # flowing this returns a full chunk immediately.
            rx_data.extend(self.serial.receive(timeout=0.001))
            rx_data = framer.parse(rx_data)
            if not frames:
                continue

            if logger:
                for frame in frames:
                    packet: Optional[LogPacket] = packetiser.parse(frame)
                    if packet:
                        logger.log(packet)
            else:
                for frame in frames:
                    print(frame)
            frames.clear()

    def reset(self):
        pass

    def timestamp_to_seconds(self, timestamp: int) -> float:
        return float(timestamp)


def transport_factory_cli(app: typer.Typer):
    @app.command(name="itm")
    def transport_factory_cb(
        ctx: typer.Context,
        port: str = typer.Argument(..., help="Serial port (eg COM12)"),
        baudrate: int = typer.Argument(..., help="TPIU baudrate"),
        elf: List[Path] = typer.Option([], help="Symbol file path (elf/out file)"),
        alias: Optional[str] = typer.Option(None, help="Alias for this device in the log"),
        pcsample: Optional[Path] = typer.Option(
            None,
            help="Write a speedscope profile of DWT PC samples to this file on "
            "exit and open the interactive viewer in the default browser",
        ),
        late_attach: bool = typer.Option(
            False,
            "--late-attach",
            help="Attach to an already-running target: byte-align on the next ITM "
            "sync packet instead of waiting for the boot reset token. Requires the "
            "device to emit sync packets (ITM_enableSyncPackets).",
        ),
    ):
        """Add ITM transport as input to log.

        You need to specify a serial port and a baudrate (ITM driver module
        configures this). The baudrate is often quite high, such as 12000000
        (12MHz).

        You also need to specify .out/.elf files that contain symbol information
        needed to parse the log, but this may also be provided globally before
        adding transports.

        DWT hardware events (PC samples, exception trace, watchpoints, counter
        wraps) appear as module DWT/ITM packets next to the Log_* records. To
        profile with PC sampling, enable ITM_enablePCSampling on the device and
        pass --pcsample out.speedscope.json.
        """

        state = ctx.ensure_object(LoggerCliCtx)
        elves = state.symbol_files + elf

        if len(elves) == 0:
            typer.secho(
                "Need elf symbols. Specify via --elf <file> on command line.", fg=typer.colors.BRIGHT_RED, err=True
            )
            sys.exit(1)

        db = TraceDB(elves, repickle=False)
        itm_transport = ITM_Transport(port, baudrate, db, alias or port, pcsample=pcsample, late_attach=late_attach)
        return itm_transport


#### Local usage below


def itm_parser_main():
    # Local finalizer for command results, not exported.
    def local_cli_finalizer(transports: List[ITM_Transport]):
        for t in transports:  # transports returned by each subcommand
            t.start(logger=None)

    local_cli = typer.Typer(name="itm_transport", result_callback=local_cli_finalizer, chain=True)
    transport_factory_cli(local_cli)
    local_cli()


def itm_raw_viewer():
    # Open raw ITM viewer based on COM-port and baudrate.
    parser = argparse.ArgumentParser(prog="itm_transport", description="View raw ITM data from auxiliary COM port")

    # Add positional argument to get COM-port
    parser.add_argument(
        "com_port",
        metavar="COM",
        type=str,
        help="Choose the COM port for the device to be debugged. Note that this must be the Auxiliary COM port.",
    )

    # Add positional argument to get baudrate
    parser.add_argument(
        "baud_rate",
        metavar="BAUD",
        type=int,
        help="Choose a baud rate for serial communication.",
    )

    # Parse arguments from command-line
    args = parser.parse_args()

    # Create an ITM transport and start it. With logger=None, the raw output will be printed to terminal
    itm_transport = ITM_Transport(args.com_port, args.baud_rate, None, None)
    itm_transport.start(logger=None)
