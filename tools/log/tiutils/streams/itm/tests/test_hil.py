"""Hardware-in-the-loop check: a real board streaming LogSinkITM over a real
probe's SWO/aux serial port, decoded live by the real transport. Skipped
cleanly unless configured; never runs in CI.

    ITM_HIL=1 ITM_HIL_PORT=/dev/ttyACM1 ITM_HIL_BAUD=12000000 \
    ITM_HIL_ELF=/path/to/app.out pytest tests/test_hil.py -s

The board must be running an application with LogSinkITM enabled (and, for
DWT/PC packets, ITM_enablePCSampling or watchpoints configured). Reset the
board after starting the test so the reset token is seen.

The flamegraph path has no automated HIL: run
    tilogger --elf <app.out> itm <port> <baud> --pcsample out.speedscope.json stdout
let it capture, hit Ctrl+C, and confirm the browser view opens and shows the
sampled functions.
"""

import os
import threading
import time

import pytest

pytestmark = pytest.mark.skipif(
    os.environ.get("ITM_HIL") != "1",
    reason="hardware-in-the-loop is opt-in: set ITM_HIL=1, ITM_HIL_PORT, ITM_HIL_BAUD, ITM_HIL_ELF",
)


def test_live_board_decodes_log_records():
    from tilogger.tracedb import TraceDB
    from tilogger_itm_transport.itm_transport import ITM_Transport

    port = os.environ["ITM_HIL_PORT"]
    baud = int(os.environ.get("ITM_HIL_BAUD", "12000000"))
    elf = os.environ["ITM_HIL_ELF"]

    class CollectingLogger:
        def __init__(self):
            self.packets = []
            self.timebase = None

        def log(self, packet):
            self.packets.append(packet)

        def get_system_time(self, device_time):
            return device_time

    db = TraceDB([elf], repickle=False)
    transport = ITM_Transport(port, baud, db, "HIL")
    sink = CollectingLogger()
    thread = threading.Thread(target=transport.start, args=(sink,), daemon=True)
    thread.start()

    # Give the user time to reset the board; the sink emits its boot sequence
    # (reset token + timing info) on LogSinkITM_init.
    deadline = time.monotonic() + float(os.environ.get("ITM_HIL_SECONDS", "20"))
    while not sink.packets and time.monotonic() < deadline:
        time.sleep(0.1)

    transport.stop()
    thread.join(timeout=2)
    if transport.serial:
        transport.serial.close()

    assert sink.packets, "no LogPackets decoded from the live board; reset the board during the window"
    for packet in sink.packets[:10]:
        print(f"{packet.timestamp_local:.6f} {packet.module}: {packet._str_data}")
