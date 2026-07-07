"""Logger routing and time-base behaviour without a live pipeline: an empty
Logger starts no threads, so its methods can be driven directly with fake
packets and outputs."""

import tilogger.logger as L
from tilogger.interface import LogLevel, LogPacket
from tilogger.logger import Logger


def _packet(module="LogMod_App", opcode=10, str_data="hi"):
    # opcode >= RESERVED_OPCODES skips the dobby-format step, so no trace_db needed.
    p = LogPacket("dev", module, opcode, LogLevel.Log_INFO, "f.c", "1", 1.0, 1.0, b"\x00\x00", None)
    p._str_data = str_data
    return p


def test_get_system_time_anchors_on_first_call():
    lg = Logger([], [])
    assert lg.get_system_time(500.0) == 500.0  # first call returns the device time
    assert lg.timebase is not None
    assert isinstance(lg.get_system_time(999.0), float)  # later calls are wall-clock


def test_log_routes_packet_to_outputs():
    class FakeOutput:
        def __init__(self):
            self.packets = []

        def start(self):  # Logger runs each output on its own thread
            pass

        def notify_packet(self, p):
            self.packets.append(p)

    out = FakeOutput()
    lg = Logger([], [out])
    p = _packet()
    lg.log(p)
    assert out.packets == [p]


def test_wait_threads_stops_transports(monkeypatch):
    # Pin the live-thread count to 1 so wait_threads falls straight through to
    # its cleanup instead of blocking on the session's background threads.
    monkeypatch.setattr(L.threading, "active_count", lambda: 1)

    class FakeTransport:
        alias = "t"

        def __init__(self):
            self.stopped = False

        def start(self, logger):
            pass

        def stop(self):
            self.stopped = True

    ft = FakeTransport()
    lg = Logger([ft], [])
    lg.wait_threads()
    assert ft.stopped
