"""Headless framer+packetiser benchmark (Task 1 decision harness).

Feeds a synthetic ITM byte stream straight into ITMFramer + ITMPacketiser in
64 KiB chunks, the way the serial loop delivers data, but with no serial port
and no inter-thread queue, so it measures the decode path itself.

Correctness is asserted on every run (frame count, LogPacket count, rolling
CRC32 over packet payloads), so a fast-but-wrong decoder fails loudly.

Run standalone:
    .venv/bin/python tests/bench_itm.py --profile dense --mb 32
    .venv/bin/python tests/bench_itm.py --profile mixed --mb 32
or via pytest (see test_bench.py):
    ITM_BENCH=1 pytest tests/test_bench.py -s

The pass bar is a sustained 24 MHz SWO line: 24 Mbit/s NRZ 8N1-style
(start + 8 data + stop) = 2.4 MB/s of ITM bytes. Decision-record numbers
live in streams/itm/ARCHITECTURE.md.
"""

from __future__ import annotations

import argparse
import resource
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import synth  # noqa: E402
from conftest import FakeLogger, FakeTraceDB, ListSink  # noqa: E402

from tilogger_itm_transport.itm_framer import ITMFramer  # noqa: E402
from tilogger_itm_transport.itm_to_log import ITMPacketiser  # noqa: E402

CHUNK = 65536
ADDR = 0x9000_0100  # dense profile: 2-arg format string
ADDR_MIXED = 0x9000_0200  # mixed profile: 1-arg format string
# The pass bar: a sustained 24 MHz SWO line. Async NRZ is 8N1-framed, so one
# byte costs 10 bit times: 24 Mbit/s -> 2.4 MB/s of ITM bytes (the 12 MHz
# hardware maximum is 1.2 MB/s). The ~3 MB/s figure quoted in docs is the
# rougher bits/8 arithmetic; measured results should clear 2.4 with margin.
BAR_MB_S = 2.4


@dataclass
class BenchResult:
    profile: str
    total_bytes: int
    seconds: float
    frames: int
    packets: int
    checksum: int
    peak_rss_kb: int

    @property
    def mb_per_s(self) -> float:
        return self.total_bytes / self.seconds / 1e6

    @property
    def frames_per_s(self) -> float:
        return self.frames / self.seconds

    def report(self) -> str:
        return (
            f"{self.profile}: {self.total_bytes / 1e6:.1f} MB in {self.seconds:.2f} s"
            f" -> {self.mb_per_s:.2f} MB/s, {self.frames_per_s / 1e6:.3f} M frames/s,"
            f" {self.packets} packets, crc 0x{self.checksum:08x},"
            f" peak RSS {self.peak_rss_kb / 1024:.1f} MB"
        )


def build_db() -> FakeTraceDB:
    db = FakeTraceDB()
    db.add_fmt(ADDR, "Count %d of %d", 2)
    db.add_fmt(ADDR_MIXED, "IRQ latency %d", 1)
    return db


def make_block(profile: str) -> tuple:
    """Returns (block_bytes, frames_per_block, packets_per_block)."""
    if profile == "dense":
        # 3 frames per record + 1 LTS every 8 records
        records = 4096
        block = synth.dense_block(records, ADDR, nargs=2, ts_every=8)
        frames = records * 3 + records // 8
        packets = records
    elif profile == "mixed":
        groups = 1024
        block = synth.mixed_block(groups, ADDR_MIXED)
        # Per group: lts, 2 pc, exc, sleep-pc, exc, trace, log hdr + 1 arg = 9
        # plus a counter wrap every 16 groups.
        frames = groups * 9 + groups // 16
        packets = groups
    else:
        raise ValueError(profile)
    return block, frames, packets


def run(profile: str, target_mb: float) -> BenchResult:
    db = build_db()
    block, frames_per_block, packets_per_block = make_block(profile)
    repeats = max(1, int(target_mb * 1e6) // len(block))

    boot = synth.sink_boot()
    boot_frames = 5  # reset + 2 info + 2 time-sync SW frames

    sink = ListSink()
    framer = ITMFramer(sink)
    packetiser = ITMPacketiser(db, FakeLogger())

    frames = 0
    packets = 0
    checksum = 0
    total = 0

    start = time.perf_counter()
    carry = bytearray()
    for rep in range(-1, repeats):
        data = boot if rep < 0 else block
        total += len(data)
        for off in range(0, len(data), CHUNK):
            carry.extend(data[off : off + CHUNK])
            carry = framer.parse(carry)
            for frame in sink:
                packet = packetiser.parse(frame)
                if packet is not None:
                    packets += 1
                    checksum = zlib.crc32(packet.data, checksum)
            frames += len(sink)
            sink.clear()
    seconds = time.perf_counter() - start

    expect_frames = boot_frames + repeats * frames_per_block
    expect_packets = repeats * packets_per_block
    # The framer legitimately holds back the last <5 bytes of the stream.
    assert frames >= expect_frames - 2, (frames, expect_frames)
    assert packets >= expect_packets - 1, (packets, expect_packets)

    return BenchResult(
        profile=profile,
        total_bytes=total,
        seconds=seconds,
        frames=frames,
        packets=packets,
        checksum=checksum,
        peak_rss_kb=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=["dense", "mixed", "both"], default="both")
    parser.add_argument("--mb", type=float, default=32.0, help="Bytes to push through, in MB")
    args = parser.parse_args()

    profiles = ["dense", "mixed"] if args.profile == "both" else [args.profile]
    ok = True
    for profile in profiles:
        result = run(profile, args.mb)
        marker = "PASS" if result.mb_per_s >= BAR_MB_S else "FAIL"
        print(f"[{marker} vs {BAR_MB_S:.1f} MB/s bar] {result.report()}")
        ok = ok and result.mb_per_s >= BAR_MB_S
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
