"""Setup/teardown tests for the Wireshark output.

The output owns the whole lifecycle on Linux/macOS: it makes a FIFO, launches
Wireshark on it, and asks the logger to stop (shutdown_requested) when the
viewer goes away - either the child exiting (watcher thread) or the FIFO write
end breaking (BrokenPipeError). These tests exercise that teardown with a
stubbed viewer (`cat`), so no Wireshark install is needed.
"""

import os
import shutil
import subprocess
import sys
import threading
import time

import pytest

from tilogger.interface import LogLevel, LogPacket
from tilogger_wireshark import main as ws_main

# The FIFO setup/teardown path under test only exists on POSIX; Windows uses
# win32 named pipes instead.
pytestmark = pytest.mark.skipif(
    sys.platform == "win32" or not hasattr(os, "mkfifo"),
    reason="POSIX FIFO teardown path does not exist on Windows",
)


def wait_for(pred, timeout=5.0):
    """Poll pred until true or timeout; bounded so a regression fails, not hangs."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(0.02)
    return pred()


def make_packet() -> LogPacket:
    """Minimal packet with every field _send reads (alias, timestamps, opcode,
    module, level, filename, lineno, _str_data)."""
    return LogPacket(
        alias="dut",
        module="LogMod_App",
        opcode=15,  # custom opcode (>= 10): _send prints the number, no enum lookup
        level=LogLevel.Log_INFO,
        filename="app.c",
        lineno="42",
        timestamp=1.5,
        timestamp_local=1.5,
        data=b"",
        trace_db=None,
    )


@pytest.fixture
def cleanup():
    """Track stub processes and outputs; kill/remove them even on assert failure."""
    items = {"procs": [], "outputs": []}
    yield items
    for proc in items["procs"]:
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    for output in items["outputs"]:
        try:
            if output._fifo is not None:
                output._fifo.close()
        except OSError:
            pass  # reader already gone; close() flush may hit EPIPE
        shutil.rmtree(os.path.dirname(output.ws_pipe), ignore_errors=True)


def start_in_thread(output) -> threading.Thread:
    # start() blocks in open(fifo, "wb") until a reader opens the FIFO, so run
    # it off-thread and let the test bound how long it may take to unblock.
    thread = threading.Thread(target=output.start, daemon=True)
    thread.start()
    return thread


def test_window_close_requests_shutdown(monkeypatch, cleanup):
    """WHY: closing the Wireshark window must stop the logger. The watcher
    thread waits on the launched child and sets shutdown_requested when it
    exits; if that breaks, the logger keeps decoding into a dead pipe forever."""
    readers = []

    def fake_start_wireshark(pipe):
        # Stand-in viewer: opens the FIFO for read so start() unblocks.
        proc = subprocess.Popen(["cat", pipe], stdout=subprocess.DEVNULL)
        readers.append(proc)
        return proc

    monkeypatch.setattr(ws_main, "start_wireshark", fake_start_wireshark)

    output = ws_main.WiresharkOutput(ws_pipe="stub")  # non-None: take the auto-launch path
    cleanup["outputs"].append(output)
    thread = start_in_thread(output)
    thread.join(timeout=5)
    assert not thread.is_alive(), "start() never unblocked; stub reader did not open the FIFO"

    reader = readers[0]
    cleanup["procs"].append(reader)
    assert output.shutdown_requested is False

    reader.kill()  # "window closed"
    reader.wait()
    assert wait_for(lambda: output.shutdown_requested), "watcher thread did not request shutdown after the viewer exited"


def test_broken_pipe_requests_shutdown(cleanup):
    """WHY: if the viewer's read end vanishes (Stop pressed, dumpcap died), the
    next packet write hits BrokenPipeError; that must request shutdown instead
    of crashing the transport thread. ws_pipe=None skips the auto-launch so no
    watcher thread can set the flag for us - only the write path is on trial."""
    output = ws_main.WiresharkOutput(ws_pipe=None)
    cleanup["outputs"].append(output)
    thread = start_in_thread(output)
    reader = subprocess.Popen(["cat", output.ws_pipe], stdout=subprocess.DEVNULL)
    cleanup["procs"].append(reader)
    thread.join(timeout=5)
    assert not thread.is_alive(), "start() never unblocked; reader did not open the FIFO"

    reader.kill()
    reader.wait()  # exited: read end closed, next write gets EPIPE
    assert output.shutdown_requested is False

    output.notify_packet(make_packet())
    assert output.shutdown_requested is True, "BrokenPipeError on the FIFO write did not request shutdown"


def test_wait_threads_stops_on_output_flag():
    """WHY: shutdown_requested only works if Logger.wait_threads actually polls
    it, returns, and stops the transports. This proves the portable stop path
    end to end with no Wireshark and no signals."""
    from tilogger.interface import TransportABC
    from tilogger.logger import Logger

    class StubTransport(TransportABC):
        def __init__(self):
            self._stopped = threading.Event()
            self.stop_called = False

        @property
        def alias(self):
            return "stub"

        def start(self, logger):
            self._stopped.wait()  # park like a real transport until stop()

        def reset(self):
            pass

        def stop(self):
            self.stop_called = True
            self._stopped.set()

        def timestamp_to_seconds(self, timestamp):
            return float(timestamp)

    class StubOutput:
        shutdown_requested = False

        def start(self):
            pass

        def notify_packet(self, packet):
            pass

    transport = StubTransport()
    output = StubOutput()
    lgr = Logger([transport], [output])

    waiter = threading.Thread(target=lgr.wait_threads, daemon=True)
    waiter.start()
    time.sleep(0.3)  # a few poll periods with the flag False
    assert waiter.is_alive(), "wait_threads returned before any shutdown was requested"

    output.shutdown_requested = True
    waiter.join(timeout=5)
    assert not waiter.is_alive(), "wait_threads did not return after an output requested shutdown"
    assert transport.stop_called, "wait_threads returned without stopping the transports"


@pytest.mark.skipif(
    not (shutil.which("wireshark") and os.environ.get("RFTRACE_WS_TEST") == "1"),
    reason="real-GUI test: needs wireshark on PATH and RFTRACE_WS_TEST=1",
)
def test_real_wireshark_close_requests_shutdown(cleanup):
    """WHY: the stubbed tests trust that a real Wireshark behaves like `cat`
    (opens the FIFO, exits on close). This opt-in test checks that against the
    actual GUI; gated off CI because it needs a display and an install."""
    output = ws_main.WiresharkOutput(ws_pipe="launch")
    cleanup["outputs"].append(output)
    thread = start_in_thread(output)
    assert wait_for(lambda: output._ws_proc is not None, timeout=10), "start() never launched Wireshark"
    cleanup["procs"].append(output._ws_proc)
    thread.join(timeout=30)  # the GUI can take a while to open the pipe
    assert not thread.is_alive(), "Wireshark never opened the FIFO for reading"

    output._ws_proc.terminate()
    assert wait_for(lambda: output.shutdown_requested, timeout=10), "watcher did not request shutdown after Wireshark exited"
