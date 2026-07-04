# tilogger ITM transport - architecture and decision record

Host-side decoder for the LogSinkITM trace path: Cortex-M SWO/ITM bytes from
a debug probe's auxiliary serial port, deframed and reassembled into TI Log_*
records and DWT hardware events, delivered to the shared tilogger outputs
(stdout, Wireshark, replay files). This note records how the pipeline is
shaped and why, with the measured numbers behind each decision, so none of it
has to be re-litigated.

## Integration spike results (done first, before any feature work)

1. Live capture rate, headless: PASS. A pseudo-terminal stand-in for the
   probe's serial port feeds the real SerialRx thread and the real
   ITM_Transport receive loop (tests/test_integration.py::
   test_rate_spike_sustained_3mbps, runs in the normal suite). Measured on
   the reference machine: 12 MB sustained at 4.09 MB/s end to end with zero
   record loss and the reader queue peaking at 2 x 64 KiB chunks (bounded
   memory). That is above the ~3 MB/s design feed and well above the true
   24 MHz byte rate (2.4 MB/s, see arithmetic below). The remaining risk is
   not host software but per-probe serial ceilings, documented in "Serial
   path limits"; the real 12/24 MHz capture off a physical probe remains the
   hardware-gated check (ITM_HIL=1, tests/test_hil.py).

2. Flamegraph viewer interop: PASS. The vendored speedscope 1.25.0 app
   (streams/itm/tilogger_itm_transport/speedscope/, MIT, ~1 MB, from the npm
   package's dist/release) opened a hand-made profile fully offline on the
   run OS (Linux, headless Chromium render verified: title, tabs, and the
   four weighted frames all drawn from file:// with no network). The open
   path is portable by construction: pathlib .resolve().as_uri() +
   webbrowser, always via a tiny redirect page because the macOS "open" and
   Windows "start" launchers drop URL fragments (same mechanism speedscope's
   own CLI ships). One-line manual check on the other OSes:
   `python -c "from tilogger_itm_transport.pcsample import *; import collections; open_in_speedscope(write_speedscope_profile(collections.Counter({('f',None,None):1}), 'x.json'))"`
   Wireshark itself has no flamegraph view (no such visualization exists in
   its UI; the lua dissector renders text columns only), so this is additive,
   not a duplicate of the Wireshark path.

## Data flow

```
serial port (probe aux UART / SWO)
  -> SerialRx thread: blocking read, 64 KiB chunks -> queue.Queue of chunks
  -> ITM_Transport.start loop (one thread):
       ITMFramer.parse(buf)      bytes -> ITMFrame objects (list sink)
       ITMPacketiser.parse(f)    frames -> LogPacket or None
       Logger.log(packet)        -> stdout / Wireshark / replay outputs
```

- itm_framer.py: byte-level deframer; the hot path. Index-cursor parse, one
  slice per frame payload, `__slots__` frame classes with lazy `__str__`.
- itm_to_log.py: Log_* record reassembly (STIM_HEADER + STIM_TRACE against
  the TraceDB), timestamp accumulation, control/time-sync frames, and the
  DWT/overflow surfacing (below).
- pcsample.py: flat PC histogram -> speedscope JSON + offline viewer open.
- serial_rx.py: reader thread. The old 0.1 ms sleep per loop with 1 KiB
  chunks was itself a ~2 MB/s structural ceiling; the read already blocks
  on the serial timeout, so the sleep is gone and chunks are 64 KiB.
- The old per-frame queue.Queue between framer and packetiser (lock round
  trip per frame, 1 ms blocking get per loop turn, one frame per turn) is
  replaced by a plain list drained in the same thread.

End-user surface is unchanged: `tilogger --elf app.out itm PORT BAUD
[outputs...]`, entry point `itm`, plus one new opt-in option `--pcsample`
(options go before the positionals, as with `--elf`/`--alias`).

## Wire format facts

Stream framing: raw ITM. The TPIU formatter is explicitly disabled by the
device driver (source/ti/drivers/ITM.c, ITM_initHw writes `FFCR = 0`), and
SWO is configured as NRZ/UART (`SPPR = 2`). There is no CoreSight formatter
frame to undo on any shipped CM3/CM4/CM33 sink configuration; if a future
sink enables the formatter (e.g. to mux ETM), a 16-byte formatter deframer
must be added in front of ITMFramer. Global timestamps are likewise never
enabled (`GTSENA` stays 0) but are decoded anyway to stay in sync.

Packet encodings (ARM DDI 0403 Appendix D4; identical in DDI 0553 for
everything the TI sinks emit):

```
SW source   header = (port << 3) | size_code           bit2 = 0
HW source   header = (disc << 3) | 0x04 | size_code    bit2 = 1
size_code   0b01 = 1 byte, 0b10 = 2, 0b11 = 4
LTS1        0b1TTT0000 + 1..4 LEB128-style payload bytes (7 bits each)
LTS2        0b0TTT0000, delta 1..6 in the header, no payload
GTS1/GTS2   0x94 / 0xB4 + LEB128-style payload (GTS2 64-bit: up to 6 bytes)
sync        >= 47 zero bytes ended by 0x80
overflow    0x70
extension   0b0PPP1S00; single byte unless bit 7 set
DWT disc    0 = counter wrap, 1 = exception trace, 2 = PC sample,
            8..23 = data trace, else reserved
data trace  disc bits [4:3]: 01 = PC value (bit0=0) / address (bit0=1),
            10 = data value read (bit0=0) / write (bit0=1);
            bits [2:1] = comparator
exception   2-byte payload: number[8:0], function bits [13:12]
            (1 = entry, 2 = exit, 3 = return)
```

TI layer on top (source/ti/log/LogSinkITM.c):

- Reset token: STIM_INFO word 0xBBBBBBBB -> bytes FB BB BB BB BB. The framer
  refuses to parse until it has seen it, and resynchronizes on it after a
  device reset mid-capture.
- STIM_HEADER (port 29): 32-bit address of the log record in .log_data;
  resolved via TraceDB. STIM_TRACE (port 28): argument words, or for Log_buf
  a length word followed by payload (4/2/1-byte writes).
- STIM_INFO (port 31): opcode 3 = timing info: prescaler code immediate
  (TCR.TSPrescale encoding: 0/1/2/3 -> divide by 1/4/16/64; the sink uses
  16) then a TimestampP_Format word.
- STIM_SYNC_TIME (port 30): 64-bit native timestamp, LSW first.

Timestamp math: local timestamps are deltas in prescaled CPU cycles;
`rtc_s += delta / (clock / prescaler)`. Frames between timestamps are spread
by their own wire time (`(size + 1) / baud`). Time sync converts the native
64-bit timestamp using the announced format word: fracBytes/intBytes are
octet counts (bits = x8), multiplier is signed 16-bit, negative multiplier
divides (one time unit is abs(multiplier) ticks), then scaled by
10^-exponent. Three host bugs were fixed here and are pinned by tests: the
format word was unpacked with native "L" (8 bytes on LP64 Linux/macOS, so
time sync never worked there), the octet counts were treated as bit widths,
and negative multipliers were read as unsigned.

## Surfaced hardware packets (previously parsed and dropped)

Every DWT event and ITM overflow now becomes a LogPacket and flows through
the unchanged output path (module/level/text synthesized here; the Wireshark
lua dissector is column-based and needed no change; the Wireshark output
plugin needed one guard because `Opcode(x)` only knows Log.h opcodes 0-3):

| event           | module | opcode | level       | text example                                        |
|-----------------|--------|--------|-------------|-----------------------------------------------------|
| PC sample       | DWT    | 10     | Log_VERBOSE | pc sample: 0x000100A6 ICall_waitMatch (icall.c:2323)|
| exception trace | DWT    | 11     | Log_INFO    | exception 15 (SysTick) entry                        |
| watchpoint      | DWT    | 12     | Log_INFO    | watchpoint 1: write, value 0x1234                   |
| counter wrap    | DWT    | 13     | Log_VERBOSE | counter wrap: CPI Cyc                               |
| ITM overflow    | ITM    | 14     | Log_WARNING | ITM overflow: the device dropped at least one ...   |

They carry the same accumulated device clock as the Log_* records, so they
interleave correctly on stdout and in Wireshark. PC samples resolve to
function (file:line) through tilogger/dwarf.py get_all_functions_range()
over the --elf files, built lazily once and cached (TraceDB.function_ranges;
about 1.5 s for a full BLE image, 2174 functions). Two real bugs in the
in-tree RangeDict made last-function lookups always miss and idx-0 misses
wrap around; both fixed and pinned. Rendered PC/exception texts are memoized
per address/event because samples repeat heavily and the formatting cost is
measurable at frame rates.

Rate note: surfacing turns every DWT event into a LogPacket, so a saturated
PC-sampling stream produces hundreds of thousands of output lines per
second; the decoder keeps up (numbers below) but stdout or the Wireshark
pipe become the practical bottleneck. For profiling, the histogram +
--pcsample path aggregates instead of printing.

## PC sampling view (--pcsample)

`tilogger --elf app.out itm --pcsample out.speedscope.json PORT BAUD stdout`
collects a histogram keyed by (function, file, line) - plus `<sleep>` for
idle samples and raw hex for unresolved PCs - and on exit (Ctrl+C) writes a
speedscope "sampled" profile (single-frame stacks weighted by sample count:
the device emits PCs, not call stacks, so this is a flat profile by design)
and opens the vendored offline viewer. Tool choice per the selection
criteria: speedscope is MIT, one static HTML/JS bundle usable from file://
on all three OSes with zero install or server; flamegraph.pl (static SVG,
non-interactive, needs perl), the Firefox Profiler (hosted app, online), and
d3-flame-graph (CDN + hand-rolled page) all lost on the offline/no-moving-
parts criteria. The JSON generator is pinned against an expected sample in
tests so a format drift is caught without a browser.

## Decision record: Python vs compiled backend (Task 1)

Method mirrors tools/rftrace-decode: a like-for-like headless benchmark
(tests/bench_itm.py) feeding synthetic byte streams straight into
framer + packetiser in 64 KiB chunks, correctness asserted on every run
(exact record count + rolling CRC32 over packet payloads). Two profiles:
dense (back-to-back 2-arg Log_printf records, worst case for reassembly)
and mixed (heavy PC sample/exception/watchpoint/wrap traffic + logs, worst
case for per-frame dispatch; every frame emits a LogPacket).

Machine: AMD Ryzen 7 7840U, Fedora 40, CPython 3.12.10, single core.
Bar arithmetic: SWO async NRZ is 8N1-framed (start + 8 + stop = 10 bit
times/byte), so 24 MHz = 2.4 MB/s of ITM bytes and the 12 MHz hardware
maximum on these parts is 1.2 MB/s. The rougher bits/8 figure (3.0 MB/s) is
kept as the stretch target in the harness output.

Measured (32 MB per run, peak RSS ~40 MB and flat in all cases):

| stage                                 | dense MB/s | mixed MB/s |
|---------------------------------------|------------|------------|
| shipped code (queue removed, fairness)| 0.93       | 1.16       |
| refactored pure Python                | 3.5 - 4.0  | 2.8 - 3.1  |
| + Cython-compiled deframe loop (spike)| 3.95       | 3.40       |

The shipped code was below even the 12 MHz line rate; the refactor (cursor
parse, no per-frame queue, slots frames, lazy strings, gated debug logging,
lookup hoists, render memoization) is 3-4x and clears the true 24 MHz rate
(2.4 MB/s) on both profiles with 17-65% margin; dense also clears the 3.0
stretch figure, mixed ties it.

Escalation was then tried, not assumed: the inner deframe loop was compiled
with Cython (tests/cyframer_spike.pyx, checksum-identical output) and bought
only ~15% on the worst profile, because the remaining cost is creating the
per-frame/per-packet Python objects that the packetiser and output layer
contract requires - which a compiled deframer cannot remove. NumPy was
rejected by analysis for the same reason (variable-length packet chain plus
mandatory per-frame Python objects; the RF tracer's NumPy variant lost
similarly). A full compiled core in the rftrace style (fusing framer +
packetiser and emitting only final records) would remove that cost, but is
not warranted: the hardware cannot exceed 12 MHz (1.2 MB/s) and pure Python
already decodes 2.3-3.3x that on every profile.

Verdict: single-language Python, no compiled backend. Revisit trigger:
hardware with SWO beyond 24 MHz, or measured sustained decode below the
line rate in the HIL check. The Cython spike stays in tests/ with build
instructions so the numbers are reproducible.

## Off-the-shelf ITM/SWO parser assessment (Task 4)

Searched and assessed (July 2026):

- Orbuculum / Orbcode (github.com/orbcode/orbuculum): BSD-3-Clause, C/C++,
  actively maintained (v2.2.0, 2024; development continues). The de-facto
  reference SWO/ITM/DWT/ETM decoder, with orbtop as a PC-sampling top view.
  It is a suite of separate processes wired over TCP, not an importable
  library; reusing it would mean subprocess plumbing plus re-implementing
  the TI Log_* layer against its output - more moving parts than the ~500
  lines it would replace, plus a native build/distribution burden for what
  is today a pip-only tool. Kept as the reference implementation to
  cross-check decodes against.
- pyOCD (pyocd.io, Apache-2.0, Python, maintained): importable ITM/SWV
  parsing in pyocd.trace.swv tied to its event-graph objects. Correct for
  the generic layer, but pulling in the whole probe stack as a dependency
  to replace ~300 lines of deframer is out of proportion, and its parser
  covers less than ours does now (no LTS2 handling of our sink's timestamp
  math, no TI framing).
- pyswo (github.com/beg0/pyswo): MIT, single author, dormant since 2023,
  never released on PyPI. Not a dependency to build a product path on.
- Rust itm / itm-decode / rtic-scope: the original rust-embedded/itm crate
  is deprecated in favor of rtic-scope/itm (MIT/Apache, sans-IO, decodes
  exactly DDI 0403 D4); rtic-scope activity has been low since ~2022. Using
  it would add a Rust toolchain or binary shipping for the same generic
  layer only.

Common to all: none understand the TI-specific layer, which is where the
actual product value is - the 0xBBBBBBBB reset token, the STIM_HEADER/
STIM_TRACE record framing against the .log_data TraceDB, the STIM_INFO
timing/format announcements, and the LogPacket integration. The generic
ITM/DWT layer they do cover is ~300 lines here and is now pinned by
spec-derived tests. Decision: keep the in-house parser; the maintenance
surface it saves is smaller than the integration surface any candidate adds.

## CM3 / CM4 / CM33 notes (Task 5)

Checked against ARMv7-M (DDI 0403, CM3/CM4) and ARMv8-M (DDI 0553, CM33);
test layouts in tests/synth.py are written from the documents, not from the
decoder, and the poc that seeded them (tools/log/itm_synth_poc.py) validated
the same encodings against a real CC27xx (CM33) image.

- Local timestamps: LTS1 and LTS2 encodings are identical on all three
  cores. LTS2 (single-byte, deltas 1..6) was previously dropped as
  "reserved", which silently skewed timing for busy streams on every core;
  fixed. The prescaler is read off the wire (STIM_INFO Info_Timing) rather
  than hard-coded per core.
- Global timestamps: identical encodings (GTS1/GTS2, GTS2 48- or 64-bit).
  Never enabled by the TI sinks; decoded to preserve sync, including the
  64-bit GTS2 length that can exceed the loop guard (deferred to the next
  read).
- Exception trace: 9-bit exception number + 2-bit function on both
  architectures. v8-M/CM33 adds SecureFault (exception 7, reserved on
  v7-M); the rendered name map covers it. The M4/M33 FPU lazy-stacking
  behavior changes when exceptions fire, not how they are encoded.
- Data trace: discriminator layout (type bits [4:3], comparator [2:1],
  direction bit 0) is the same; the 2-bit comparator field caps trace
  attribution at comparators 0..3 on every core, which also covers CM33
  (4 comparators on these parts). v8-M reserves some v7-M data-trace match
  variants, which shrinks - not changes - the packet space; reserved
  discriminators (3..7, 24..31) are now skipped by their encoded length
  instead of corrupting the stream.
- PC samples: 4-byte PC / 1-byte sleep variant, identical.
- TPIU formatter: disabled by the shipped driver on all cores (FFCR = 0,
  see above); the raw-ITM assumption is explicit and stated here.

## Serial path limits (per probe / per OS, from vendor data)

The pipeline decodes 2.4+ MB/s, but the serial link in front of it has its
own ceilings; do not read the software rate as universal:

- XDS110: SWO is UART/NRZ format only; TI reports about 6 Mbaud as the
  practical SWO/aux UART maximum (E2E support statements; no hard number in
  the probe documentation), i.e. ~0.6 MB/s. The aux UART has no hardware
  flow control, so an over-saturated link drops bytes inside the probe where
  the host cannot see it - the on-wire overflow packet and the reset-token
  resync are the only recovery signals.
- ST-Link: SWO capture is ~2 MHz on v2 and ~24 MHz on v3 (ST documentation).
- J-Link: model-dependent, tens of MHz on the higher-end probes.
- Plain USB-UART bridges: FT232R tops out at 3 Mbaud; FT2232H/FT232H reach
  12 Mbaud. The FTDI latency timer (default 16 ms) batches small transfers;
  it affects latency, not throughput, and the 64 KiB blocking reads here are
  insensitive to it.
- Baud plausibility is not validated by pyserial: Linux accepts arbitrary
  rates via termios BOTHER, macOS via IOSSIOSPEED; the OS driver silently
  delivers whatever the probe actually produces, so a mismatched rate shows
  up as garbage before the reset token (discarded) rather than as an error.
- macOS: the historical read(1)-before-in_waiting workaround in serial_rx.py
  is retained (one byte consumed pre-reset-token is harmless), but the
  receive path no longer depends on in_waiting at all - it uses blocking
  reads with a timeout - so the quirk it guarded is out of the hot path.

## Tests and how to run everything

```
cd tools/log/tiutils/streams/itm
../../.venv/bin/python -m pytest tests/            # full suite, ~3 s
ITM_BENCH=1 [ITM_BENCH_MB=64] .../pytest tests/test_bench.py -s   # benchmark
python tests/bench_itm.py --profile dense --mb 32  # benchmark, standalone
ITM_SPIKE_MB=64 .../pytest tests/test_integration.py -k rate -s   # longer soak
ITM_HIL=1 ITM_HIL_PORT=/dev/ttyACM1 ITM_HIL_BAUD=12000000 \
  ITM_HIL_ELF=app.out .../pytest tests/test_hil.py -s             # live board
```

- tests/synth.py: spec-derived encoder for every packet type plus the TI
  layer; the byte layouts come from DDI 0403/0553 and LogSinkITM.c, so
  round-trips are conformance checks, not tautologies.
- tests/test_framer.py: per-frame-type decode, raw-viewer strings pinned
  byte-for-byte, split-buffer sweeps (chunk sizes 1..64), reset-token
  handling including splits, spec-conformance section.
- tests/test_packetiser.py: record reassembly (printf + buffer opcode),
  timestamp/prescaler/resync math, DWT surfacing, symbolization against a
  mocked RangeDict, and the byte-exact stdout/Wireshark regression pins that
  gated the hot-path refactor.
- tests/test_integration.py: full mixed round trip in wire order, DWT
  rendering on the real outputs, and the pty rate proof.
- tests/test_pcsample.py: profile JSON pinned, vendored viewer presence,
  portable open path.
- The Log_* tests run against a synthetic TraceDB (no .out committed); the
  seed poc tools/log/itm_synth_poc.py additionally validates the same
  encoders against a real application image when given one.
