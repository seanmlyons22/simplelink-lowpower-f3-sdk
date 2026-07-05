"""Throughput benchmark for the LogSinkBuf host path.

LogSinkBuf is a RAM-polled ring, not a real wire: the target CPU writes RAM
directly and the host only reads it over SWD. "Faster than the wire" means
decode throughput must exceed SWD read throughput, so decode is never the
bottleneck and the only ceiling is the probe.

Always measured:  pure-Python decode MB/s over a large synthetic ring.
With a probe   :  TILOGGER_BUF_HIL=1 python bench_buf.py also measures
                  sustained pyOCD read_memory_block32 MB/s from target RAM.

Decision rule (see README): if the measured pyOCD read throughput clears the
target log fill rate with margin, pyOCD stays. If pyOCD is too slow, evaluate
probe-rs behind the same MemoryReader interface and record the comparison.
"""

import os
import time

from tilogger.bufdecode import RingWriter, decode_buffer
from tilogger.tracedb import ElfString

# Nominal SWD read throughput the decode side must beat. pyOCD over
# CMSIS-DAP/J-Link sustains well under 1 MB/s of RAM reads in practice
# (the SWD physical ceiling at 32 MHz SWCLK is ~4 MB/s and is never
# approached), so 1.0 is already a generous stand-in for "the wire".
WIRE_MBPS_NOMINAL = 1.0

# ponytail: fixed SRAM base for the read benchmark; both CC23xx and CC27xx
# map SRAM at 0x20000000.
RAM_BASE = 0x20000000


def _make_elf(fmt, nargs, opcode="LOG_OPCODE_FORMATED_TEXT"):
    value = "\x1e".join([opcode, "file.c", "42", "Log_DEBUG", "LogMod_Bench", fmt, str(nargs)])
    return ElfString(value, None)


INDEX = {
    0x000C: _make_elf("x=%d", 1),
    0x0010: _make_elf("a=%d b=%d c=%d", 3),
}


def measure_decode(ring_bytes=4 << 20):
    """Fill a synthetic ring with RingWriter and time one decode_buffer pass.

    Returns (MB_per_s, records_per_s, decoded).
    """
    writer = RingWriter(size=ring_bytes)
    i = 0
    while writer.wr_reserve < ring_bytes - 64:
        writer.printf(0x000C, i, (i,))
        writer.printf(0x0010, i, (i, i * 3, 0xFFFFFFFF))
        i += 3

    t0 = time.perf_counter()
    res = decode_buffer(writer.buf, writer.wr_reserve, writer.last_ts, writer.rec_count, INDEX)
    dt = time.perf_counter() - t0
    assert res.torn == 0 and res.decoded == writer.rec_count
    return writer.wr_reserve / dt / 1e6, res.decoded / dt, res.decoded


def measure_probe_read(span=16384, repeats=64, probe=None, target="cortex_m"):
    """Sustained pyOCD block-read throughput from target RAM, in MB/s.

    Needs a probe attached; gate callers behind TILOGGER_BUF_HIL=1.
    """
    from tilogger_buf_transport.memory import PyocdReader

    reader = PyocdReader(probe=probe, target=target)
    reader.read(RAM_BASE, span)  # connect + warm up outside the timed loop
    t0 = time.perf_counter()
    for _ in range(repeats):
        reader.read(RAM_BASE, span)
    dt = time.perf_counter() - t0
    reader.close()
    return span * repeats / dt / 1e6


if __name__ == "__main__":
    decode_mbps, recs_per_s, decoded = measure_decode()
    print("decode: %.2f MB/s (%.0f records/s over %d records)" % (decode_mbps, recs_per_s, decoded))
    print("nominal SWD wire: %.2f MB/s -> decode is %.1fx the wire" % (WIRE_MBPS_NOMINAL, decode_mbps / WIRE_MBPS_NOMINAL))

    if os.environ.get("TILOGGER_BUF_HIL") == "1":
        read_mbps = measure_probe_read()
        print("pyOCD read: %.3f MB/s measured" % read_mbps)
        print("decode is %.1fx the measured wire" % (decode_mbps / read_mbps))
        if read_mbps >= decode_mbps:
            print("WARNING: probe reads outrun decode; time to evaluate probe-rs (see README)")
    else:
        print("set TILOGGER_BUF_HIL=1 with a probe attached to also measure pyOCD read throughput")
