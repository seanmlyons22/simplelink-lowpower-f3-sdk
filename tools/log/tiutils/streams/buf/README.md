# tilogger buf transport (LogSinkBuf)

Host reader for the LogSinkBuf sink: the target packs each log record into a
circular RAM byte buffer (COBS-framed, ULEB-coded, ~5-9 bytes per record) and
runs happily with no host attached. This transport polls that buffer over SWD
(or reads it out of a saved RAM dump), decodes it with the shared core in
`tilogger.bufdecode`, and feeds the records into the normal `tilogger`
pipeline, so they render **identically to ITM/UART logs** on stdout, in
Wireshark, and in replay files.

The host is strictly read-only: it never halts the core and never writes
anything back. Falling behind is safe; overwritten records are counted and
reported as `drops=N`.

## Install

```sh
# dump replay only (no probe library needed):
pip install -e streams/buf

# live probe support (pulls pyOCD):
pip install -e streams/buf[probe]
```

After install, `tilogger --help` lists the `buf` subcommand.

## Run modes

Live probe, stdout:

```sh
tilogger --elf app.out buf stdout
```

Live probe, Wireshark (same unchanged Lua dissector as ITM/UART):

```sh
tilogger --elf app.out buf stdout wireshark --start
```

RAM dump replay (post-mortem; decodes the newest intact window once, then
exits):

```sh
tilogger --elf app.out buf --dump ram.bin --base 0x20000000 stdout
```

Standalone dump viewer without the pipeline:

```sh
python -m tilogger_buf_transport ram.bin --elf app.out
```

## Options

| option | default | meaning |
|---|---|---|
| `--instance NAME` | `CONFIG_ti_log_LogSinkBuf_0` | sink instance; the struct symbol read is `LogSinkBuf_<NAME>_config` (the default is the SysConfig singleton) |
| `--probe UID` | first probe found | pyOCD probe unique id |
| `--target TYPE` | `cortex_m` | pyOCD target type. CC23xx/CC27xx have no guaranteed pyOCD device pack; the generic `cortex_m` target reads RAM fine, which is all this transport does |
| `--dump FILE` | | decode a saved RAM dump instead of attaching a probe |
| `--base ADDR` | `0x20000000` | load address of the dump (SRAM base on CC23xx/CC27xx) |
| `--poll SEC` | `0.01` | poll interval. Falling behind is not an error; laps are counted as drops |
| `--alias NAME` | `buf` | stream name shown in outputs |

Capturing a dump: halt or don't, then e.g.
`pyocd cmd -t cortex_m -c "savemem 0x20000000 0x9000 ram.bin"` (size >= your
RAM), or the equivalent CCS/GDB memory save. The dump must cover both the
`LogSinkBuf_<NAME>_config` struct and the buffer it points to.

## Behavior notes

- **Attach-late / post-mortem**: the first poll reads the whole ring once and
  decodes the newest intact window; a partially overwritten oldest record is
  realigned away, not miscounted.
- **Reboot**: a backward jump in `wrReserve`/`recCount` resets decode state
  and re-attaches; a forward lap (host too slow) is *not* a reset, it is
  counted as drops.
- **Tearing**: a record overwritten mid-read fails COBS/consistency checks
  and is counted in `torn`; the decoder resyncs at the next frame boundary.
- **Probe loss**: reads retry with backoff and reconnect; the thread reports
  instead of dying silently.
- `drops=N torn=M` tallies go to stderr, throttled to ~1/s.

## Performance (decided by measurement, not assertion)

LogSinkBuf is a RAM-polled ring: the target CPU writes RAM directly, so "the
wire" is SWD read bandwidth. The requirement is that host decode outruns the
wire, so the probe is the only ceiling. `scripts/bench_buf.py` measures both:

```sh
python streams/buf/scripts/bench_buf.py                    # decode MB/s (always)
TILOGGER_BUF_HIL=1 python streams/buf/scripts/bench_buf.py # + pyOCD read MB/s
```

Measured on a dev box: decode ~2.2 MB/s / ~180k records/s pure Python.
pyOCD sustains well under 1 MB/s of RAM reads in practice, and at ~5-9 bytes
per record even a 1 kHz log rate is under 10 kB/s, so decode clears the wire
with a wide margin. The poll loop keeps the probe side minimal: one 28-byte
struct read plus block reads of only the fresh byte span per poll (two reads
when the span wraps), all via `read_memory_block32`, never halting.

**Decision rule**: if measured pyOCD read throughput clears your target's log
fill rate with margin, pyOCD stays. If pyOCD is too slow, evaluate `probe-rs`
(Rust core, markedly faster SWD memory reads) behind the same one-method
`MemoryReader` interface in `memory.py` and record the measured comparison --
it is a drop-in swap, no transport changes.

## Tests

```sh
pytest streams/buf/tests -v
```

All synthetic (a `RingWriter` mirrors the target C byte-for-byte), including
the identical-to-ITM rendering assertion and the decode-vs-wire benchmark
floor. With a board attached:

```sh
TILOGGER_BUF_HIL=1 TILOGGER_BUF_ELF=app.out pytest streams/buf/tests -v
```
