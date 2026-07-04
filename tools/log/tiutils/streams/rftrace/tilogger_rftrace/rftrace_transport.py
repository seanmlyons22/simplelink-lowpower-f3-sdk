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

"""RF-core (LRFDTRC) trace transport for tilogger.

Swaps in for the `itm`/`uart` transports: instead of a serial port it runs the
native `tracedecode` backend (the Rust logic-analyzer decoder) and adapts its
output into `LogPacket`s, so every existing tilogger output (`stdout`,
`wireshark`, `to-replayfile`, ...) works unchanged.

    python -m tilogger rftrace --sal capture.sal --dbgid app_dbgid.h \
        --dbgid pbe_dbgid.h --channel 4 --divide-time-by-2 wireshark --start
    python -m tilogger rftrace --sal capture.sal --dbgid app_dbgid.h stdout
    python -m tilogger rftrace --sigrok "--driver saleae-logic-pro-16 \
        --config samplerate=500M -C D4 --samples 500M" --dbgid app_dbgid.h stdout

The decoder already resolves dbgids and formats the text; we transport its
canonical `||` pcap payload (the same bytes `tilogger_dissector.lua` reads) back
into pre-formatted `LogPacket`s, exactly like the `from-replayfile` transport.

Only stdlib is imported at module top so the pcap adapter stays unit-testable
without the tilogger/typer stack (see `_selfcheck`); tilogger + typer are
imported lazily inside the CLI/transport.
"""

import logging
import os
import platform
import shlex
import struct
import subprocess
import sys
import time

logger = logging.getLogger("RFTrace Transport")

# Rust `tracedecode` emits level "INFO" (dbgids carry no level); map onto tilogger's
# LogLevel names so stdout/wireshark render identically to ITM ("Log_INFO", ...).
_LEVEL_NAMES = {
    "DEBUG": "Log_DEBUG",
    "VERBOSE": "Log_VERBOSE",
    "INFO": "Log_INFO",
    "WARNING": "Log_WARNING",
    "ERROR": "Log_ERROR",
}

PCAP_GLOBAL_HEADER_LEN = 24
PCAP_RECORD_HEADER_LEN = 16

# Subclass tilogger's TransportABC so the logger CLI's `isinstance(s, TransportABC)` filter
# adds us as an input stream. Fall back to `object` when tilogger isn't importable, so the
# stdlib-only pcap adapter + self-check below still run standalone.
try:
    from tilogger.interface import TransportABC as _TransportBase
except Exception:  # pragma: no cover - only when running the self-check outside tilogger
    _TransportBase = object


def _read_exact(stream, n):
    """Read exactly n bytes from a (possibly short-reading) pipe; <n only at EOF."""
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def parse_pcap_records(stream):
    """Yield (ts_seconds: float, columns: list[str]) for each record in a
    classic-pcap DLT_USER0 stream whose payload is tilogger's `||` text.

    `columns` are the 8 dissector fields; a text field containing `||` is
    rejoined so it survives the split.
    """
    ghdr = _read_exact(stream, PCAP_GLOBAL_HEADER_LEN)
    if len(ghdr) < PCAP_GLOBAL_HEADER_LEN:
        return
    while True:
        rhdr = _read_exact(stream, PCAP_RECORD_HEADER_LEN)
        if len(rhdr) < PCAP_RECORD_HEADER_LEN:
            break
        ts_s, ts_us, caplen, _orig = struct.unpack("<IIII", rhdr)
        payload = _read_exact(stream, caplen)
        if len(payload) < caplen:
            break
        fields = payload.decode("utf-8", "replace").split("||")
        if len(fields) < 8:
            continue
        # Prefer the payload's own .9f device time (fields[1]); fall back to header.
        try:
            ts = float(fields[1])
        except ValueError:
            ts = ts_s + ts_us / 1_000_000.0
        columns = fields[:7] + ["||".join(fields[7:])]
        yield ts, columns


def _resolve_tracedecode(explicit):
    """Locate the `tracedecode` binary: --tracedecode, $TRACEDECODE, or PATH."""
    import shutil

    if explicit:
        return str(explicit)
    env = os.getenv("TRACEDECODE")
    if env:
        return env
    found = shutil.which("tracedecode")
    if found:
        return found
    return "tracedecode"  # let the OS raise a clear "not found" if it isn't there


def _build_command(td, common, sal, raw):
    if sal:
        return [td, "replay", str(sal), *common, "pcap", "--out", "-"]
    if raw:
        return [td, "decode", "--raw", str(raw), *common, "pcap", "--out", "-"]
    return None  # sigrok handled separately (two-process pipe)


class RFTrace_Transport(_TransportBase):
    """tilogger transport that fronts the native `tracedecode` backend."""

    def __init__(self, *, tracedecode, sal, raw, sigrok, channel, samplerate, baud,
                 divide_time_by_2, dbgid, elf, alias,
                 logic2=False, duration=None, trigger=None, trigger_channel=None,
                 after=None, buffer_mb=None, loop=False, count=None,
                 logic2_port=10430, logic2_address="127.0.0.1", logic2_device=None,
                 threshold=1.8):
        self._td = _resolve_tracedecode(tracedecode)
        self._sal = sal
        self._raw = raw
        self._sigrok = sigrok
        self._channel = channel
        self._samplerate = samplerate
        self._baud = baud
        self._divide = divide_time_by_2
        self._dbgid = list(dbgid)
        self._elf = elf
        self._alias = alias or "rftrc"
        self._procs = []
        self._elf_tmp = None  # temp DBG_DEF file extracted from --elf
        # --logic2: drive Logic 2 (Automation API) in a capture loop; each window is
        # saved to a temp .sal and fed through the same replay path. Batched, not
        # streaming (the Pro 8 + Automation API can't stream mid-capture).
        self._logic2 = logic2
        self._duration = duration
        self._trigger = trigger
        self._trigger_channel = trigger_channel
        self._after = after
        self._buffer_mb = buffer_mb
        self._loop = loop
        self._count = count
        self._l2_port = logic2_port
        self._l2_address = logic2_address
        self._l2_device = logic2_device
        self._threshold = threshold
        self._stopping = False

    @property
    def alias(self):
        return self._alias

    def _common_args(self):
        args = ["--channel", str(self._channel), "--alias", self._alias]
        for d in self._dbgid:
            args += ["--dbgid", str(d)]
        if self._samplerate is not None:
            args += ["--samplerate", str(self._samplerate)]
        if self._baud is not None:
            args += ["--baud", str(self._baud)]
        if self._divide:
            args += ["--divide-time-by-2"]
        return args

    def _spawn(self):
        """Start the decoder (and sigrok, if live) and return its pcap stdout stream."""
        common = self._common_args()
        if self._sigrok is not None:
            sig = subprocess.Popen(
                shlex.split("sigrok-cli " + self._sigrok) + ["-O", "binary"],
                stdout=subprocess.PIPE,
            )
            dec = subprocess.Popen(
                [self._td, "decode", "--raw", "-", *common, "pcap", "--out", "-"],
                stdin=sig.stdout,
                stdout=subprocess.PIPE,
            )
            sig.stdout.close()  # let sig receive SIGPIPE if dec dies
            self._procs = [sig, dec]
            return dec.stdout

        cmd = _build_command(self._td, common, self._sal, self._raw)
        if cmd is None:
            raise SystemExit("rftrace: need one of --sal, --raw, or --sigrok")
        dec = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        self._procs = [dec]
        return dec.stdout

    def start(self, logger_core):
        from tilogger.interface import LogPacket, LogLevel
        from tilogger.tracedb import Opcode

        # --elf: extract the CPU-side dbgid table straight from the ELF (no manual elf2dbgid).
        # It goes first so any explicit --dbgid (modem/LRF: pbe/rfe/mce) layers on top.
        if self._elf is not None:
            from .elf_dbgid import elf_to_dbgid_file
            self._elf_tmp = elf_to_dbgid_file(self._elf)
            self._dbgid = [self._elf_tmp] + self._dbgid

        def emit(stream):
            for ts, cols in parse_pcap_records(stream):
                alias, _ts_str, _opcode, module, level, filename, lineno, text = cols
                level_name = _LEVEL_NAMES.get(level.upper(), "Log_INFO")
                packet = LogPacket(
                    alias=alias,
                    module=module,
                    # REPLAY_FILE opcode => logger.log() skips ELF re-formatting (we have none).
                    opcode=Opcode.REPLAY_FILE.value,
                    level=LogLevel[level_name],
                    filename=filename,
                    lineno=lineno,
                    timestamp=ts,
                    timestamp_local=ts,
                    data=None,
                    trace_db=None,
                )
                packet._str_data = text
                if logger_core:
                    logger_core.log(packet)
                else:
                    print(f"{alias} | {ts:.9f} | {module} | {level_name} | {filename}:{lineno} | {text}")

        try:
            if self._logic2:
                self._run_logic2(emit)
            else:
                emit(self._spawn())
                self._reap()
        finally:
            if self._elf_tmp is not None:
                try:
                    os.unlink(self._elf_tmp)
                except OSError:
                    pass

    def _reap(self):
        for p in self._procs:
            try:
                p.wait(timeout=10)
            except Exception:
                p.kill()
        self._procs = []

    def _capture_config(self, automation):
        """Build a CaptureConfiguration from the --duration / --trigger knobs."""
        if self._trigger:
            tt = {
                "rising": automation.DigitalTriggerType.RISING,
                "falling": automation.DigitalTriggerType.FALLING,
                "pulse-high": automation.DigitalTriggerType.PULSE_HIGH,
                "pulse-low": automation.DigitalTriggerType.PULSE_LOW,
            }[self._trigger]
            ch = self._trigger_channel if self._trigger_channel is not None else self._channel
            mode = automation.DigitalTriggerCaptureMode(
                trigger_type=tt,
                trigger_channel_index=ch,
                after_trigger_seconds=self._after if self._after is not None else 1.0,
            )
        else:
            mode = automation.TimedCaptureMode(duration_seconds=self._duration)
        return automation.CaptureConfiguration(
            capture_mode=mode, buffer_size_megabytes=self._buffer_mb
        )

    def _pick_device(self, mgr, automation):
        if self._l2_device:
            return self._l2_device
        # Logic 2's automation port starts listening a beat before the USB device
        # is enumerated, so right after a fresh (headless) launch get_devices() is
        # briefly empty. Poll instead of bailing - otherwise this transport thread
        # SystemExits silently and the whole capture tears down at startup.
        deadline = time.time() + 15
        while True:
            devs = [d for d in mgr.get_devices() if not getattr(d, "is_simulation", False)]
            if devs:
                break
            if time.time() >= deadline:
                raise SystemExit("rftrace --logic2: no Saleae device found "
                                 "(is Logic 2 running with the device connected?)")
            time.sleep(0.5)
        pros = (automation.DeviceType.LOGIC_PRO_8, automation.DeviceType.LOGIC_PRO_16)
        for d in devs:
            if d.device_type in pros:
                return d.device_id
        return devs[0].device_id

    def _run_logic2(self, emit):
        """Loop: capture a window via the Logic 2 Automation API, save it to a temp
        .sal, and decode it through the same replay path - repeat per --loop/--count."""
        import tempfile
        try:
            from saleae import automation
        except ImportError:
            raise SystemExit("rftrace --logic2 needs the Saleae automation package: "
                             "pip install logic2-automation")

        sr = int(self._samplerate) if self._samplerate else 500_000_000
        devcfg = automation.LogicDeviceConfiguration(
            enabled_digital_channels=[self._channel],
            digital_sample_rate=sr,
            digital_threshold_volts=self._threshold,
        )
        mgr = automation.Manager.connect(address=self._l2_address, port=self._l2_port)
        try:
            device_id = self._pick_device(mgr, automation)
            n = 0
            while not self._stopping:
                cap = mgr.start_capture(
                    device_id=device_id,
                    device_configuration=devcfg,
                    capture_configuration=self._capture_config(automation),
                )
                fd, tmp = tempfile.mkstemp(prefix="rftrace_l2_", suffix=".sal")
                os.close(fd)
                try:
                    cap.wait()  # blocks until the timed window / trigger+after completes
                    cap.save_capture(tmp)
                finally:
                    try:
                        cap.close()
                    except Exception:
                        pass
                self._sal = tmp
                try:
                    emit(self._spawn())
                    self._reap()
                finally:
                    try:
                        os.unlink(tmp)
                    except OSError:
                        pass
                n += 1
                if self._count and n >= self._count:
                    break
                if not self._loop and not self._count:
                    break  # single capture unless --loop / --count
        except KeyboardInterrupt:
            pass
        finally:
            try:
                mgr.close()
            except Exception:
                pass

    def stop(self):
        self._stopping = True
        for p in self._procs:
            try:
                p.terminate()
            except Exception:
                pass

    def reset(self):
        pass

    def timestamp_to_seconds(self, timestamp):
        return float(timestamp)


def transport_factory_cli(app):
    import typer
    from pathlib import Path
    from typing import List, Optional
    from tilogger.interface import LoggerCliCtx

    @app.command(name="rftrace")
    def transport_factory_cb(
        ctx: typer.Context,
        elf: Optional[Path] = typer.Option(None, "--elf", help="application .out; CPU-side logs are read straight from it (no manual elf2dbgid)"),
        dbgid: List[Path] = typer.Option([], "--dbgid", help="extra DBG_DEF header(s) for modem/LRF (pbe/rfe/mce); repeatable"),
        sal: Optional[Path] = typer.Option(None, "--sal", help="Saleae .sal capture to replay"),
        raw: Optional[Path] = typer.Option(None, "--raw", help="raw 1 byte/sample file, or - for stdin"),
        sigrok: Optional[str] = typer.Option(None, "--sigrok", help="sigrok-cli args for live capture, piped into the decoder"),
        logic2: bool = typer.Option(False, "--logic2", help="capture via the Logic 2 Automation API (Saleae Logic Pro), looped"),
        duration: Optional[float] = typer.Option(None, "--duration", help="[--logic2] timed capture window, seconds"),
        trigger: Optional[str] = typer.Option(None, "--trigger", help="[--logic2] digital trigger: rising|falling|pulse-high|pulse-low"),
        trigger_channel: Optional[int] = typer.Option(None, "--trigger-channel", help="[--logic2] trigger channel (default: --channel)"),
        after: Optional[float] = typer.Option(None, "--after", help="[--logic2] seconds to capture after the trigger (default 1.0)"),
        buffer_mb: Optional[int] = typer.Option(None, "--buffer-mb", help="[--logic2] capture RAM buffer bound, MB"),
        loop: bool = typer.Option(False, "--loop", help="[--logic2] keep capturing windows until stopped"),
        count: Optional[int] = typer.Option(None, "--count", help="[--logic2] capture exactly N windows then stop"),
        logic2_port: int = typer.Option(10430, "--logic2-port", help="[--logic2] automation server port"),
        logic2_address: str = typer.Option("127.0.0.1", "--logic2-address", help="[--logic2] automation server address"),
        logic2_device: Optional[str] = typer.Option(None, "--logic2-device", help="[--logic2] device id (default: first Logic Pro found)"),
        threshold: float = typer.Option(1.8, "--threshold", help="[--logic2] digital threshold volts"),
        channel: int = typer.Option(4, "--channel", help="LA channel of the rfctrc_out pin"),
        samplerate: Optional[float] = typer.Option(None, "--samplerate", help="override sample rate (Hz)"),
        baud: Optional[float] = typer.Option(None, "--baud", help="override tracer baud (Hz)"),
        divide_time_by_2: bool = typer.Option(False, "--divide-time-by-2", help="48 MHz tracer / 0.25 us ticks (e.g. CC2745)"),
        alias: Optional[str] = typer.Option(None, "--alias", help="device alias shown in the log"),
        tracedecode: Optional[Path] = typer.Option(None, "--tracedecode", help="path to the tracedecode binary ($TRACEDECODE or PATH otherwise)"),
    ):
        """Add the RF-core (LRFDTRC) trace decoder as a log input.

        Drop-in for `itm`/`uart`: pick a source and one or more outputs, e.g.

            tilogger rftrace --sal cap.sal --elf app.out \\
                --channel 4 --divide-time-by-2 wireshark --start

        Metadata: pass `--elf <app.out>` to resolve all CPU-side logs straight from the
        binary (no manual elf2dbgid step). Add `--dbgid <file>` for modem/LRF traces
        (pbe/rfe/mce), which aren't in the app ELF. At least one of --elf / --dbgid is required.

        Sources (exactly one): --sal <file>, --raw <file|->, --sigrok "<args>", or
        --logic2 (drive Logic 2's Automation API to capture from a Saleae Logic Pro;
        needs `pip install logic2-automation` and Logic 2 running with automation
        enabled). With --logic2 set --duration <s> or --trigger <edge>; add --loop or
        --count N for rolling windows. Batched, not continuous (the hardware can't stream).

            tilogger rftrace --logic2 --duration 2 --loop --elf app.out \\
                --channel 4 --divide-time-by-2 wireshark --start
            tilogger rftrace --logic2 --trigger rising --trigger-channel 4 --after 1 \\
                --count 5 --elf app.out stdout
        """
        ctx.ensure_object(LoggerCliCtx)
        nsources = sum(x is not None for x in (sal, raw, sigrok)) + (1 if logic2 else 0)
        if nsources != 1:
            typer.secho("rftrace: specify exactly one source: --sal, --raw, --sigrok, or --logic2", fg=typer.colors.BRIGHT_RED, err=True)
            raise typer.Exit(2)
        if logic2:
            if trigger is None and duration is None:
                typer.secho("rftrace --logic2: need --duration <s> or --trigger <edge>", fg=typer.colors.BRIGHT_RED, err=True)
                raise typer.Exit(2)
            if trigger is not None and trigger not in ("rising", "falling", "pulse-high", "pulse-low"):
                typer.secho("rftrace --trigger: one of rising|falling|pulse-high|pulse-low", fg=typer.colors.BRIGHT_RED, err=True)
                raise typer.Exit(2)
        if elf is None and not dbgid:
            typer.secho("rftrace: need --elf <app.out> and/or --dbgid <file>", fg=typer.colors.BRIGHT_RED, err=True)
            raise typer.Exit(2)
        return RFTrace_Transport(
            tracedecode=tracedecode, sal=sal, raw=raw, sigrok=sigrok, channel=channel,
            samplerate=samplerate, baud=baud, divide_time_by_2=divide_time_by_2,
            dbgid=dbgid, elf=elf, alias=alias,
            logic2=logic2, duration=duration, trigger=trigger, trigger_channel=trigger_channel,
            after=after, buffer_mb=buffer_mb, loop=loop, count=count,
            logic2_port=logic2_port, logic2_address=logic2_address,
            logic2_device=logic2_device, threshold=threshold,
        )


# ---- Local runner + self-check (no tilogger/typer needed) ----


def _selfcheck():
    """Build a 1-record DLT_USER0 pcap in memory and assert the adapter parses it."""
    import io

    payload = "rftrc||0.007969500||LOG_OPCODE_FORMATED_TEXT||DBGCH1||INFO||RCL.c||884||RCL_open: Git SHA:  abc".encode()
    ghdr = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 256, 147)
    rhdr = struct.pack("<IIII", 0, 7969, len(payload), len(payload))
    stream = io.BytesIO(ghdr + rhdr + payload)

    recs = list(parse_pcap_records(stream))
    assert len(recs) == 1, recs
    ts, cols = recs[0]
    assert abs(ts - 0.007969500) < 1e-12, ts
    assert cols[0] == "rftrc"
    assert cols[3] == "DBGCH1"
    assert cols[5] == "RCL.c"
    assert cols[6] == "884"
    assert cols[7] == "RCL_open: Git SHA:  abc", cols[7]
    assert _LEVEL_NAMES.get(cols[4].upper()) == "Log_INFO"

    # text containing '||' must survive
    p2 = "a||1.0||o||m||INFO||f.c||1||x||y".encode()
    s2 = io.BytesIO(ghdr + struct.pack("<IIII", 0, 0, len(p2), len(p2)) + p2)
    _, c2 = list(parse_pcap_records(s2))[0]
    assert c2[7] == "x||y", c2[7]

    # If the Saleae automation package is present, exercise the --logic2 config builder.
    try:
        from saleae import automation
    except ImportError:
        pass
    else:
        def _mk(**kw):
            base = dict(tracedecode=None, sal=None, raw=None, sigrok=None, channel=4,
                        samplerate=None, baud=None, divide_time_by_2=False, dbgid=["x"],
                        elf=None, alias=None, logic2=True)
            base.update(kw)
            return RFTrace_Transport(**base)
        c = _mk(duration=2.0)._capture_config(automation)
        assert isinstance(c.capture_mode, automation.TimedCaptureMode)
        assert c.capture_mode.duration_seconds == 2.0
        c = _mk(trigger="rising", after=0.5, buffer_mb=256)._capture_config(automation)
        assert isinstance(c.capture_mode, automation.DigitalTriggerCaptureMode)
        assert c.capture_mode.trigger_type == automation.DigitalTriggerType.RISING
        assert c.capture_mode.after_trigger_seconds == 0.5
        assert c.buffer_size_megabytes == 256
        print("rftrace_transport logic2 config self-check OK")

    print("rftrace_transport self-check OK")


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
    else:
        print(__doc__)
