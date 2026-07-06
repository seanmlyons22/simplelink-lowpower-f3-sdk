# tilogger ITM transport - runbook

Host decoder for the LogSinkITM SWO/ITM trace path. This is the "how do I run
it later" card. For why it is built this way (wire format, the Python-vs-Rust
decision with numbers, per-core notes), see `ARCHITECTURE.md`.

## One-time setup

The prebuilt venv already has everything installed:

```
cd tools/log/tiutils
.venv/bin/python -m tilogger --help
```

To rebuild from scratch in a fresh venv:

```
python3 -m venv .venv
.venv/bin/pip install -e core -e streams/itm -e streams/stdout \
    -e streams/wireshark -e streams/to_replayfile -e streams/from_replayfile
```

The speedscope viewer used by `--pcsample` is vendored in
`streams/itm/tilogger_itm_transport/speedscope/`, so nothing else to install.

## Capture from a board

Serial port is the probe's auxiliary/SWO UART (XDS110 "Auxiliary Data Port",
a J-Link SWO port, an ST-Link, or an FTDI bridge). Options go before the
positional PORT and BAUD. Capture starts once the device reset frame is seen,
so reset the board after launching.

```
# logs to the terminal
tilogger --elf app.out itm PORT 12000000 stdout

# logs to Wireshark (auto-launched and configured)
tilogger --elf app.out itm PORT 12000000 wireshark --start

# record to a replay file, view it later with no board attached
tilogger --elf app.out itm PORT 12000000 to-replayfile --file cap.json
tilogger from-replayfile cap.json stdout
```

### Attaching to an already-running target (`--late-attach`)

The boot reset frame is sent once, so an attach after boot normally sees
nothing until you reset the board. `--late-attach` instead byte-aligns on the
next ITM synchronization packet (a run of zero bytes ended by `0x80`), so no
reset is needed:

```
tilogger --elf app.out itm --late-attach PORT 12000000 stdout
```

This needs the device to emit sync packets: call
`ITM_enableSyncPackets(ITM_SYNC_TAP_BIT24)` on the target. The tap is off
CYCCNT, which only advances while the core is awake, so a mostly-sleeping app
(e.g. a BLE peripheral between advertising events) emits them rarely and
late-attach can take a long time or stall. On such targets a one-shot board
reset (which re-sends the boot frame the framer also aligns on) is the reliable
path. A busy/torture target emits sync packets promptly and late-attach is
immediate.

DWT hardware events (PC samples, exception entry/exit/return, watchpoints,
counter wraps) and ITM overflow warnings show up next to the `Log_*` records
as module `DWT`/`ITM`, symbolized from the `--elf` file, on the same clock.

Raw ITM view (no ELF, prints each decoded frame): `tilogger_itm_viewer PORT BAUD`.

## PC-sample profiling (flamegraph view)

Enable the periodic sampler on the device (`ITM_enablePCSampling`), then:

```
tilogger --elf app.out itm --pcsample profile.speedscope.json PORT 12000000 stdout
```

On Ctrl+C it writes `profile.speedscope.json` and opens the offline speedscope
view in your browser. The device emits program counters, not call stacks, so
the view is a flat per-function histogram (weight = sample count). Works
offline on Linux/macOS/Windows from one code path.

## Tests

```
cd tools/log/tiutils/streams/itm
../../.venv/bin/python -m pytest tests/          # full suite, ~3 s, no hardware
```

All tests run on synthetic byte streams built from the ARM spec; no capture
file and no board are needed.

## Benchmark (Python-vs-Rust decision harness)

Gated so ordinary `pytest` stays fast; asserts a record-count + CRC invariant
on every run so it doubles as a stress test.

```
cd tools/log/tiutils/streams/itm
ITM_BENCH=1 ../../.venv/bin/python -m pytest tests/test_bench.py -s
ITM_BENCH=1 ITM_BENCH_MB=64 ../../.venv/bin/python -m pytest tests/test_bench.py -s
# or standalone:
../../.venv/bin/python tests/bench_itm.py --profile dense --mb 32
../../.venv/bin/python tests/bench_itm.py --profile mixed --mb 32
```

The Cython spike that measured the compiled-backend alternative lives in
`tests/cyframer_spike.pyx` with build/run steps in its header; it is not built
by default.

## Hardware-in-the-loop check (needs a board)

Skipped cleanly unless configured, never runs in CI:

```
ITM_HIL=1 ITM_HIL_PORT=/dev/ttyACM1 ITM_HIL_BAUD=12000000 \
    ITM_HIL_ELF=app.out ../../.venv/bin/python -m pytest tests/test_hil.py -s
```

Start it, then reset the board during the window so the reset frame is seen.

## If nothing decodes

- No output at all: the reset frame is only sent on device startup - reset the
  board after launching `tilogger`.
- Garbage then nothing: baud mismatch. pyserial/the OS do not reject a wrong
  rate; bytes before the reset token are discarded, so a wrong baud looks like
  silence. Match the baud the ITM driver configured.
- Wrong COM port on XDS110: use the Auxiliary Data Port, not the primary one.
- Dropped records under heavy trace: the probe's aux UART has no flow control
  (XDS110 tops out around 6 Mbaud); an `ITM overflow` line means the device
  dropped data before the host saw it. See `ARCHITECTURE.md` "Serial path
  limits" for per-probe ceilings.
