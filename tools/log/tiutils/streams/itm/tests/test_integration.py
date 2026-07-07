"""Integration tests: full synthetic round trips through framer + packetiser,
DWT packets rendered next to Log_* records on the real outputs, and the
Task-0 live-capture rate proof (pseudo-terminal + the real SerialRx and
transport loop at the 24 MHz-equivalent byte rate)."""

import os
import pty
import threading
import time

import synth
from conftest import FakeTraceDB, packetise

FLUSH = b"\xff" * 5


def full_stream() -> bytes:
    s = bytearray(synth.sink_boot())
    s += synth.lts(3000)
    s += synth.pc_sample(0xDEADBEEF)
    s += synth.log_record(0x9000_0100, [7, 9])
    s += synth.exception(15, synth.EXC_ENTER)
    s += synth.trace_data(1, True, 0x1234)
    s += synth.exception(15, synth.EXC_EXIT)
    s += synth.lts(1500)
    s += synth.counter_wrap(0x01)
    s += synth.overflow()
    s += synth.log_record(0x9000_0200)
    s += FLUSH
    return bytes(s)


def test_full_mixed_roundtrip_ordering(db):
    """Logs, exceptions, watchpoints, PC samples, wraps and overflow come out
    interleaved in wire order with monotonic local timestamps."""
    packets = packetise(full_stream(), db)
    assert [(p.module, p.opcode) for p in packets] == [
        ("DWT", 10),
        ("LogMod_App", 0),
        ("DWT", 11),
        ("DWT", 12),
        ("DWT", 11),
        ("DWT", 13),
        ("ITM", 14),
        ("LogMod_App", 0),
    ]
    texts = [p._str_data for p in packets if p.module in ("DWT", "ITM")]
    assert texts == [
        "pc sample: 0xDEADBEEF",
        "exception 15 (SysTick) entry",
        "watchpoint 1: write, value 0x1234",
        "exception 15 (SysTick) exit",
        "counter wrap: Cyc",
        "ITM overflow: the device dropped at least one trace packet",
    ]
    stamps = [p.timestamp_local for p in packets]
    assert stamps == sorted(stamps)


def test_dwt_renders_on_stdout(db, capsys):
    from tilogger_stdout.main import StdoutOutput, DEFAULT_LOGGING_SCHEME

    s = synth.sink_boot() + synth.lts(3000) + synth.pc_sample(0xDEADBEEF) + FLUSH
    (packet,) = packetise(s, db)
    StdoutOutput(DEFAULT_LOGGING_SCHEME, column_padding=False).notify_packet(packet)
    assert capsys.readouterr().out.splitlines() == [
        "ITM0 | 0.001000417 | DWT | Log_VERBOSE | dwt:0 | pc sample: 0xDEADBEEF"
    ]


def test_dwt_renders_in_wireshark_payload(db):
    """Custom opcodes must not break the pcap column writer (no dissector
    change: the lua side is opcode-agnostic)."""
    from tilogger_wireshark.main import WiresharkOutput

    s = synth.sink_boot() + synth.lts(3000) + synth.pc_sample(0xDEADBEEF) + FLUSH
    (packet,) = packetise(s, db)
    ws = WiresharkOutput(ws_pipe=None)
    ws.notify_packet(packet)
    (record,) = ws._pipe_backlog
    payload = record[16:].decode()
    assert payload == "ITM0||0.001000417||10||DWT||Log_VERBOSE||dwt||0||pc sample: 0xDEADBEEF"


# ---------------------------------------------------------------------------
# Task-0 spike: live capture path at the 24 MHz-equivalent byte rate,
# headless. A pseudo-terminal stands in for the probe's CDC/serial port; the
# real SerialRx thread and the real ITM_Transport receive loop do the work.
# ---------------------------------------------------------------------------


class CountingLogger:
    """Minimal Logger stand-in: counts packets, no output formatting."""

    def __init__(self):
        self.count = 0
        self.timebase = None

    def log(self, packet) -> None:
        self.count += 1

    def get_system_time(self, device_time: float) -> float:
        return device_time


def test_rate_spike_sustained_3mbps():
    """The host software must absorb a sustained ~3 MB/s ITM byte stream (the
    24 MHz SWO design target; the 12 MHz hardware maximum is 1.5 MB/s) with
    bounded memory and zero loss. ITM_SPIKE_MB enlarges the soak."""
    from tilogger_itm_transport.itm_transport import ITM_Transport

    total_mb = float(os.environ.get("ITM_SPIKE_MB", "12"))
    db = FakeTraceDB()
    db.add_fmt(0x9000_0100, "Count %d of %d", 2)

    records_per_block = 4096
    block = synth.dense_block(records_per_block, 0x9000_0100)
    repeats = max(1, int(total_mb * 1e6) // len(block))
    expected_packets = repeats * records_per_block

    master_fd, slave_fd = pty.openpty()
    transport = ITM_Transport(os.ttyname(slave_fd), 12000000, db, "SPIKE")
    sink = CountingLogger()

    decoder = threading.Thread(target=transport.start, args=(sink,), daemon=True)
    decoder.start()
    # Wait for SerialRx to open the slave and set raw mode before feeding, or
    # the pty line discipline would mangle the first bytes.
    deadline = time.monotonic() + 5
    while transport.serial is None and time.monotonic() < deadline:
        time.sleep(0.01)
    assert transport.serial is not None

    total_bytes = 0
    start = time.perf_counter()

    def feed():
        nonlocal total_bytes
        data = synth.sink_boot() + block * repeats + FLUSH
        view = memoryview(data)
        while view:
            written = os.write(master_fd, view[:65536])
            total_bytes += written
            view = view[written:]

    feeder = threading.Thread(target=feed, daemon=True)
    feeder.start()

    max_queue = 0
    deadline = time.monotonic() + 100
    while sink.count < expected_packets and time.monotonic() < deadline:
        max_queue = max(max_queue, transport.serial._rxq.qsize())
        time.sleep(0.005)
    elapsed = time.perf_counter() - start

    transport.stop()
    decoder.join(timeout=2)
    transport.serial.close()
    os.close(master_fd)
    os.close(slave_fd)

    # Zero loss: every record fed came out as a LogPacket.
    assert sink.count == expected_packets
    # Bounded memory: the reader queue never ran away (64 KiB chunks).
    assert max_queue < 128, f"reader queue peaked at {max_queue} chunks"

    rate = total_bytes / elapsed / 1e6
    print(f"\nrate spike: {total_bytes / 1e6:.1f} MB in {elapsed:.2f} s -> {rate:.2f} MB/s, queue peak {max_queue}")
    assert rate >= 3.0, f"pipeline sustained only {rate:.2f} MB/s"
