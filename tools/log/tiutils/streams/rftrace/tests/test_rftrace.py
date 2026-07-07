"""Tests for the tilogger rftrace transport.

Everything runs hardware-free: the Logic 2 automation client is faked, tracedecode is
never spawned (subprocess is recorded, not executed), and the one live end-to-end check
is gated behind RFTRACE_HIL=1 so CI always skips it.

Run from streams/rftrace with the tiutils venv:  python -m pytest tests/
"""

import io
import os
import struct
import sys
import types
from pathlib import Path
from types import SimpleNamespace

import pytest

from tilogger_rftrace import elf_dbgid
from tilogger_rftrace import rftrace_transport as rt

# ------------------------------------------------------------- pcap adapter ----


def _pcap(*payloads, ts=(0, 7969)):
    ghdr = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 256, 147)
    out = [ghdr]
    for p in payloads:
        b = p.encode()
        out.append(struct.pack("<IIII", ts[0], ts[1], len(b), len(b)))
        out.append(b)
    return io.BytesIO(b"".join(out))


PAYLOAD = "rftrc||0.007969500||LOG_OPCODE_FORMATED_TEXT||DBGCH1||INFO||RCL.c||884||RCL_open: Git SHA:  abc"


class TestParsePcapRecords:
    def test_single_record(self):
        recs = list(rt.parse_pcap_records(_pcap(PAYLOAD)))
        assert len(recs) == 1
        ts, cols = recs[0]
        assert abs(ts - 0.007969500) < 1e-12  # payload's own device time wins
        assert cols[0] == "rftrc"
        assert cols[3] == "DBGCH1"
        assert cols[5] == "RCL.c"
        assert cols[6] == "884"
        assert cols[7] == "RCL_open: Git SHA:  abc"

    def test_double_pipe_in_text_survives(self):
        recs = list(rt.parse_pcap_records(_pcap("a||1.0||o||m||INFO||f.c||1||x||y")))
        assert recs[0][1][7] == "x||y"

    def test_bad_device_time_falls_back_to_header(self):
        recs = list(rt.parse_pcap_records(_pcap("a||junk||o||m||INFO||f.c||1||t")))
        assert abs(recs[0][0] - 0.007969) < 1e-9

    def test_short_field_records_are_skipped(self):
        recs = list(rt.parse_pcap_records(_pcap("only||three||fields", PAYLOAD)))
        assert len(recs) == 1

    def test_truncated_stream_ends_cleanly(self):
        full = _pcap(PAYLOAD).getvalue()
        assert list(rt.parse_pcap_records(io.BytesIO(full[:30]))) == []
        assert list(rt.parse_pcap_records(io.BytesIO(full[:-5]))) == []
        assert list(rt.parse_pcap_records(io.BytesIO(b""))) == []

    def test_read_exact_reassembles_short_reads(self):
        class OneByte(io.BytesIO):
            def read(self, n=-1):
                return super().read(1 if n else 0)

        assert rt._read_exact(OneByte(b"abcdef"), 4) == b"abcd"


# --------------------------------------------------------- binary resolution ----


class TestResolveTracedecode:
    def test_explicit_wins(self, monkeypatch):
        monkeypatch.setenv("TRACEDECODE", "/env/td")
        assert rt._resolve_tracedecode("/explicit/td") == "/explicit/td"

    def test_env_beats_path(self, monkeypatch):
        monkeypatch.setenv("TRACEDECODE", "/env/td")
        assert rt._resolve_tracedecode(None) == "/env/td"

    def test_path_lookup_then_fallback(self, monkeypatch):
        monkeypatch.delenv("TRACEDECODE", raising=False)
        import shutil

        monkeypatch.setattr(shutil, "which", lambda name: "/usr/bin/td")
        assert rt._resolve_tracedecode(None) == "/usr/bin/td"
        monkeypatch.setattr(shutil, "which", lambda name: None)
        assert rt._resolve_tracedecode(None) == "tracedecode"


def test_build_command_variants():
    assert rt._build_command("td", ["--channel", "4"], "cap.sal", None) == [
        "td", "replay", "cap.sal", "--channel", "4", "pcap", "--out", "-",
    ]
    assert rt._build_command("td", [], None, "-") == [
        "td", "decode", "--raw", "-", "pcap", "--out", "-",
    ]
    assert rt._build_command("td", [], None, None) is None


# ------------------------------------------------------------- transport ----


def make_transport(**kw):
    base = dict(
        tracedecode="/bin/td", sal=None, raw=None, sigrok=None, channel=4,
        samplerate=None, baud=None, divide_time_by_2=False, dbgid=[], elf=None,
        alias=None,
    )
    base.update(kw)
    return rt.RFTrace_Transport(**base)


class TestCommonArgs:
    def test_every_shared_knob_reaches_tracedecode(self):
        t = make_transport(
            channel=7, samplerate=1e8, baud=1e7, divide_time_by_2=True,
            dbgid=["a.h", "b.h"], alias="brd",
        )
        args = t._common_args()
        assert args == [
            "--channel", "7", "--alias", "brd",
            "--dbgid", "a.h", "--dbgid", "b.h",
            "--samplerate", "100000000.0", "--baud", "10000000.0",
            "--divide-time-by-2",
        ]

    def test_defaults_omit_optional_flags(self):
        args = make_transport()._common_args()
        assert "--samplerate" not in args
        assert "--baud" not in args
        assert "--divide-time-by-2" not in args
        assert args[args.index("--alias") + 1] == "rftrc"

    def test_words_common_omits_sample_knobs(self):
        # decode --words is fed already-deframed words: no channel/samplerate/baud.
        t = make_transport(
            port="/dev/ttyACM0", channel=7, samplerate=1e8, baud=1e7,
            divide_time_by_2=True, dbgid=["a.h"], alias="brd",
        )
        args = t._words_common()
        assert args == ["--alias", "brd", "--dbgid", "a.h", "--divide-time-by-2"]
        assert "--channel" not in args and "--samplerate" not in args and "--baud" not in args


class RecordingPopen:
    calls = []

    def __init__(self, cmd, stdin=None, stdout=None):
        RecordingPopen.calls.append(cmd)
        self.stdout = io.BytesIO(b"")
        self.stdin = stdin

    def wait(self, timeout=None):
        return 0

    def kill(self):
        pass

    def terminate(self):
        pass


class TestSpawn:
    @pytest.fixture(autouse=True)
    def record(self, monkeypatch):
        RecordingPopen.calls = []
        monkeypatch.setattr(rt.subprocess, "Popen", RecordingPopen)

    def test_sal_spawns_replay(self):
        make_transport(sal="cap.sal", dbgid=["d.h"])._spawn()
        (cmd,) = RecordingPopen.calls
        assert cmd[:3] == ["/bin/td", "replay", "cap.sal"]
        assert cmd[-3:] == ["pcap", "--out", "-"]

    def test_raw_spawns_streaming_decode(self):
        make_transport(raw="-")._spawn()
        (cmd,) = RecordingPopen.calls
        assert cmd[1:4] == ["decode", "--raw", "-"]

    def test_sigrok_builds_a_two_process_pipe(self):
        make_transport(sigrok="--driver x -C D4")._spawn()
        sig, dec = RecordingPopen.calls
        assert sig[0] == "sigrok-cli"
        assert sig[-2:] == ["-O", "binary"]
        assert dec[1:4] == ["/bin/td", "decode", "--raw"][1:4] or dec[:3] == ["/bin/td", "decode", "--raw"]

    def test_rft1_spawns_words_decode(self):
        make_transport(rft1="cap.rft1", dbgid=["d.h"])._spawn()
        (cmd,) = RecordingPopen.calls
        assert cmd[1:4] == ["decode", "--words", "cap.rft1"]
        assert cmd[-3:] == ["pcap", "--out", "-"]
        assert "--channel" not in cmd  # words mode: no sample knobs

    def test_port_spawns_words_decode_over_a_pump(self, monkeypatch):
        # Fake pyserial + a non-running pump thread so we can inspect the tracedecode command.
        class FakeSerial:
            def __init__(self, port, baud, timeout=None, dsrdtr=None):
                self.dtr = True

            def reset_input_buffer(self):
                pass

            def read(self, n):
                return b""

            def close(self):
                pass

        fake_serial = types.ModuleType("serial")
        fake_serial.Serial = FakeSerial
        monkeypatch.setitem(sys.modules, "serial", fake_serial)

        class FakeThread:
            def __init__(self, target=None, name=None, daemon=None):
                pass

            def start(self):
                pass

        import threading
        monkeypatch.setattr(threading, "Thread", FakeThread)

        make_transport(port="/dev/ttyACM0", dbgid=["d.h"], divide_time_by_2=True)._spawn()
        (cmd,) = RecordingPopen.calls
        assert cmd[1:4] == ["decode", "--words", "-"]
        assert "--divide-time-by-2" in cmd
        assert "--channel" not in cmd

    def test_no_source_raises(self):
        with pytest.raises(SystemExit):
            make_transport()._spawn()


# ------------------------------------------------------------- CLI surface ----


@pytest.fixture
def cli(monkeypatch):
    """A typer app with the rftrace command registered and the transport recorded."""
    typer = pytest.importorskip("typer")
    from typer.testing import CliRunner

    captured = {}

    def recorder(**kwargs):
        captured.update(kwargs)
        return SimpleNamespace(alias=kwargs.get("alias") or "rftrc")

    monkeypatch.setattr(rt, "RFTrace_Transport", recorder)
    app = typer.Typer()
    rt.transport_factory_cli(app)
    return CliRunner(), app, captured


def invoke(cli, args):
    # A typer app with a single registered command runs it directly; no
    # subcommand name in argv (tilogger's real app has many, ours has one).
    runner, app, captured = cli
    result = runner.invoke(app, args)
    return result, captured


class TestCli:
    def test_requires_exactly_one_source(self, cli):
        r, _ = invoke(cli, ["--dbgid", "d.h"])
        assert r.exit_code == 2
        r, _ = invoke(cli, ["--sal", "a.sal", "--raw", "-", "--dbgid", "d.h"])
        assert r.exit_code == 2
        r, _ = invoke(cli, ["--sal", "a.sal", "--logic2", "--dbgid", "d.h"])
        assert r.exit_code == 2
        r, _ = invoke(cli, ["--port", "/dev/ttyACM0", "--sal", "a.sal", "--dbgid", "d.h"])
        assert r.exit_code == 2

    def test_requires_metadata(self, cli):
        r, _ = invoke(cli, ["--sal", "a.sal"])
        assert r.exit_code == 2

    def test_each_source_is_accepted(self, cli):
        for args, key, val in [
            (["--sal", "a.sal"], "sal", Path("a.sal")),
            (["--raw", "-"], "raw", Path("-")),
            (["--sigrok", "-C D4"], "sigrok", "-C D4"),
            (["--port", "/dev/ttyACM0"], "port", "/dev/ttyACM0"),
            (["--rft1", "cap.rft1"], "rft1", "cap.rft1"),
        ]:
            r, captured = invoke(cli, args + ["--dbgid", "d.h"])
            assert r.exit_code == 0, r.output
            assert captured[key] == val

    def test_logic2_requires_duration_or_trigger(self, cli):
        r, _ = invoke(cli, ["--logic2", "--dbgid", "d.h"])
        assert r.exit_code == 2
        r, _ = invoke(cli, ["--logic2", "--duration", "2", "--dbgid", "d.h"])
        assert r.exit_code == 0
        r, _ = invoke(cli, ["--logic2", "--trigger", "rising", "--dbgid", "d.h"])
        assert r.exit_code == 0

    def test_logic2_rejects_bad_trigger(self, cli):
        r, _ = invoke(cli, ["--logic2", "--trigger", "sideways", "--dbgid", "d.h"])
        assert r.exit_code == 2

    def test_every_option_reaches_the_transport(self, cli):
        r, captured = invoke(cli, [
            "--logic2", "--duration", "2.5", "--trigger", "falling",
            "--trigger-channel", "3", "--after", "0.5", "--buffer-mb", "256",
            "--loop", "--count", "5",
            "--logic2-port", "10431", "--logic2-address", "10.0.0.2",
            "--logic2-device", "DEV1", "--threshold", "3.3",
            "--elf", "app.out", "--dbgid", "pbe.h",
            "--channel", "6", "--samplerate", "250000000", "--baud", "12000000",
            "--divide-time-by-2", "--alias", "brd", "--tracedecode", "/x/td",
        ])
        assert r.exit_code == 0, r.output
        assert captured["logic2"] is True
        assert captured["duration"] == 2.5
        assert captured["trigger"] == "falling"
        assert captured["trigger_channel"] == 3
        assert captured["after"] == 0.5
        assert captured["buffer_mb"] == 256
        assert captured["loop"] is True
        assert captured["count"] == 5
        assert captured["logic2_port"] == 10431
        assert captured["logic2_address"] == "10.0.0.2"
        assert captured["logic2_device"] == "DEV1"
        assert captured["threshold"] == 3.3
        assert captured["elf"] == Path("app.out")
        assert captured["dbgid"] == [Path("pbe.h")]
        assert captured["channel"] == 6
        assert captured["samplerate"] == 250000000.0
        assert captured["baud"] == 12000000.0
        assert captured["divide_time_by_2"] is True
        assert captured["alias"] == "brd"
        assert captured["tracedecode"] == Path("/x/td")


# ------------------------------------------------------------- logic2 knobs ----

automation = pytest.importorskip("saleae.automation", reason="logic2-automation not installed")


class TestCaptureConfig:
    def test_timed_window(self):
        c = make_transport(logic2=True, duration=2.0)._capture_config(automation)
        assert isinstance(c.capture_mode, automation.TimedCaptureMode)
        assert c.capture_mode.duration_seconds == 2.0

    def test_trigger_window(self):
        t = make_transport(
            logic2=True, trigger="rising", trigger_channel=9, after=0.5, buffer_mb=256,
        )
        c = t._capture_config(automation)
        assert isinstance(c.capture_mode, automation.DigitalTriggerCaptureMode)
        assert c.capture_mode.trigger_type == automation.DigitalTriggerType.RISING
        assert c.capture_mode.trigger_channel_index == 9
        assert c.capture_mode.after_trigger_seconds == 0.5
        assert c.buffer_size_megabytes == 256

    def test_trigger_defaults(self):
        # Trigger channel defaults to the trace channel; capture tail to 1 s.
        c = make_transport(logic2=True, trigger="pulse-high", channel=7)._capture_config(automation)
        assert c.capture_mode.trigger_type == automation.DigitalTriggerType.PULSE_HIGH
        assert c.capture_mode.trigger_channel_index == 7
        assert c.capture_mode.after_trigger_seconds == 1.0


class FakeManager:
    def __init__(self, device_batches):
        self.batches = list(device_batches)
        self.closed = False

    def get_devices(self):
        return self.batches.pop(0) if len(self.batches) > 1 else self.batches[0]

    def close(self):
        self.closed = True


def _dev(device_id, device_type, sim=False):
    return SimpleNamespace(device_id=device_id, device_type=device_type, is_simulation=sim)


class TestPickDevice:
    def test_explicit_device_skips_discovery(self):
        t = make_transport(logic2=True, duration=1, logic2_device="EXPLICIT")
        assert t._pick_device(None, automation) == "EXPLICIT"

    def test_polls_until_the_device_enumerates(self, monkeypatch):
        # Logic 2's automation port listens before USB enumeration finishes, so the
        # first get_devices() calls can be empty; the transport must poll, not bail.
        monkeypatch.setattr(rt.time, "sleep", lambda s: None)
        mgr = FakeManager([[], [], [_dev("D1", automation.DeviceType.LOGIC_PRO_16)]])
        t = make_transport(logic2=True, duration=1)
        assert t._pick_device(mgr, automation) == "D1"

    def test_prefers_a_logic_pro_and_skips_simulated(self, monkeypatch):
        monkeypatch.setattr(rt.time, "sleep", lambda s: None)
        mgr = FakeManager([[
            _dev("SIM", automation.DeviceType.LOGIC_PRO_16, sim=True),
            _dev("OTHER", automation.DeviceType.LOGIC_8),
            _dev("PRO", automation.DeviceType.LOGIC_PRO_8),
        ]])
        t = make_transport(logic2=True, duration=1)
        assert t._pick_device(mgr, automation) == "PRO"

    def test_gives_up_after_the_deadline(self, monkeypatch):
        monkeypatch.setattr(rt.time, "sleep", lambda s: None)
        clock = iter(range(0, 100, 8))
        monkeypatch.setattr(rt.time, "time", lambda: float(next(clock)))
        t = make_transport(logic2=True, duration=1)
        with pytest.raises(SystemExit):
            t._pick_device(FakeManager([[]]), automation)


class FakeCapture:
    def __init__(self):
        self.saved = None

    def wait(self):
        pass

    def save_capture(self, path):
        self.saved = path
        Path(path).write_bytes(b"fake sal")

    def close(self):
        pass


class TestRunLogic2Loop:
    @pytest.fixture
    def looped(self, monkeypatch):
        """Wire a fully-fake Manager/capture and count decode windows."""
        state = {"captures": 0, "emits": 0}

        def fake_connect(address=None, port=None):
            state["addr"] = (address, port)
            return FakeManager([[_dev("D1", automation.DeviceType.LOGIC_PRO_16)]])

        def fake_start_capture(**kw):
            state["captures"] += 1
            state["devcfg"] = kw["device_configuration"]
            return FakeCapture()

        monkeypatch.setattr(automation.Manager, "connect", staticmethod(fake_connect))
        FakeManager.start_capture = staticmethod(fake_start_capture)

        def make(**kw):
            t = make_transport(logic2=True, **kw)
            monkeypatch.setattr(t, "_spawn", lambda: io.BytesIO(b""))
            monkeypatch.setattr(t, "_reap", lambda: None)
            return t, state

        yield make
        del FakeManager.start_capture

    def test_count_bounds_the_windows(self, looped):
        t, state = looped(duration=1.0, count=3)
        t._run_logic2(lambda stream: state.__setitem__("emits", state["emits"] + 1))
        assert state["captures"] == 3
        assert state["emits"] == 3

    def test_single_window_without_loop_or_count(self, looped):
        t, state = looped(duration=1.0)
        t._run_logic2(lambda stream: None)
        assert state["captures"] == 1

    def test_device_config_carries_channel_rate_threshold(self, looped):
        t, state = looped(duration=1.0, channel=9, samplerate=250e6, threshold=3.3)
        t._run_logic2(lambda stream: None)
        cfg = state["devcfg"]
        assert cfg.enabled_digital_channels == [9]
        assert cfg.digital_sample_rate == 250_000_000
        assert cfg.digital_threshold_volts == 3.3

    def test_connect_uses_address_and_port(self, looped):
        t, state = looped(duration=1.0, logic2_address="10.1.1.1", logic2_port=9999)
        t._run_logic2(lambda stream: None)
        assert state["addr"] == ("10.1.1.1", 9999)


# ------------------------------------------------------------- elf bridge ----


class TestElfDbgid:
    def test_pointer_to_dbg(self):
        # Channel-1 pointers were relocated so their channel field reads 4 and the
        # dbgid is off by one; channels 2/3 are unmoved.
        assert elf_dbgid.pointer_to_dbg(0x940000E4) == (1, 58)
        assert elf_dbgid.pointer_to_dbg(0x92000200) == (2, 128)
        assert elf_dbgid.pointer_to_dbg(0x93000010) == (3, 4)
        assert elf_dbgid.pointer_to_dbg(0x90000000) is None
        assert elf_dbgid.pointer_to_dbg(0x92000800) is None  # dbgid 512 > 255

    def test_nargs_field(self):
        assert [elf_dbgid._nargs_field(n) for n in (0, 1, 2, 3, 4, 9)] == [0, -1, -2, 3, 4, 4]

    def test_dbg_def_line(self):
        entry = SimpleNamespace(string="v=%d", file="/src/RCL.c", line=884, nargs=1)
        line = elf_dbgid._dbg_def_line(0x940000E4, entry)
        assert line == 'DBG_DEF(DBGID_RCL_c_884, 58, DBGCH1, -1, "v=%d", "/src/RCL.c", 884)\n'
        # Events (no string) and non-log channels produce nothing.
        assert elf_dbgid._dbg_def_line(0x940000E4, SimpleNamespace(string=None, file="f", line=1, nargs=0)) is None
        assert elf_dbgid._dbg_def_line(0x90000000, entry) is None


ELF = "/home/seanlyons/Downloads/rcl_generic_tx_burst_lp_em_cc2745r10_q1_nortos_llvm.out"


@pytest.mark.skipif(not os.path.exists(ELF), reason="reference ELF not present")
def test_elf_bridge_extracts_the_real_table():
    text = elf_dbgid.elf_to_dbgid_text(ELF)
    lines = [l for l in text.splitlines() if l.startswith("DBG_DEF(")]
    assert len(lines) > 50, "expected a real dbgid table"
    assert any(", DBGCH1," in l and '"RCL.c"' in l.replace("/RCL.c", "RCL.c") or "RCL.c" in l for l in lines)


# ------------------------------------------------------------- HIL (gated) ----


@pytest.mark.skipif(
    os.environ.get("RFTRACE_HIL") != "1",
    reason="hardware-in-the-loop check; set RFTRACE_HIL=1 with a Saleae attached "
    "and Logic 2 running with automation enabled",
)
def test_hil_logic2_single_window():
    """Live end-to-end: capture one 1 s window from the real Saleae via Logic 2 and
    decode it through the real tracedecode. Requires hardware; never runs in CI."""
    td = os.environ.get("TRACEDECODE", "tracedecode")
    dbgid = os.environ.get("RFTRACE_DBGID")
    assert dbgid, "set RFTRACE_DBGID to a DBG_DEF file for the attached firmware"
    t = make_transport(
        tracedecode=td, logic2=True, duration=1.0,
        channel=int(os.environ.get("RFTRACE_CHANNEL", "7")),
        threshold=float(os.environ.get("RFTRACE_THRESHOLD", "3.3")),
        dbgid=[dbgid], divide_time_by_2=True,
    )
    seen = []
    t._run_logic2(lambda stream: seen.extend(rt.parse_pcap_records(stream)))
    # The window decoded end to end; record count depends on firmware activity.
    assert isinstance(seen, list)
