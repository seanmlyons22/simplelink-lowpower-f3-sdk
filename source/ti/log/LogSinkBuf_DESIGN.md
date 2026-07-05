# LogSinkBuf design + overhead (ADR)

Status: as-built. Applies to `source/ti/log/LogSinkBuf.{c,h}` and the host reader
`tools/log/tiutils/streams/buf` + `core/tilogger/bufdecode.py`.

## Context

`LogSinkBuf` captures `Log_printf`/`Log_buf` records into a RAM buffer that a host
reads out of band over SWD (or from a post-mortem RAM dump). It must:

- run with **no host attached** (fill, then keep the newest history),
- cost as little **RAM per record** as possible (RAM is the scarce resource; the
  format-string metadata is already free -- see below),
- keep the **interrupt-disabled critical section tiny** (logging happens from
  driver/ISR context), and
- never halt or perturb the target (the host only reads).

## Decisions

1. **Variable-length COBS byte ring, not fixed records.** Each record is
   `COBS([id:2 LE][ULEB ts-delta][ULEB args...]) + 0x00`. The trailing `0x00` is
   both the frame delimiter and the commit marker: a torn/overwritten frame fails
   COBS decode and the host resyncs at the next `0x00`. There is no read pointer,
   no done/gen bits, no wrap padding. Replaces the old fixed 36-byte
   `LogSinkBuf_Rec`, giving **~5-9 bytes/record** (0-arg = 5 B) -- roughly **5x**
   more history per RAM byte.

2. **Circular overwrite, no read pointer.** Works with no host attached: the ring
   fills and then drops the oldest kept record. A pure RTT read-pointer model was
   rejected because host-less operation would stall.

3. **16-bit log-site id, not a 32-bit pointer.** The sink transmits the low 16
   bits of the `.log_ptr` slot address. The host maps id -> format string from the
   ELF, so no base address is hard-coded on either side. Unique up to ~16382 sites
   (host build fails loudly on a `& 0xFFFF` collision past that).

4. **ULEB delta timestamps + a `lastTs` anchor.** Records store a ULEB128 delta
   from the previous record; the instance holds `lastTs` (absolute) which the host
   reads on connect and back-sums. The RTC is read *outside* the CS; inside the CS
   a monotonic clamp (`now >= lastTs ? now-lastTs : 0`) keeps order under
   preemption and folds the ~9.5 h RTC wrap into a single zero delta.

5. **Near-zero critical section.** A single `HwiP_disable` region (PRIMASK on
   CM0+, BASEPRI on CM33, via DPL -- no `#ifdef`) does ALU only: the delta clamp,
   the reservation (`wrReserve += frameLen`), and `recCount++`. Timestamp read,
   ULEB/COBS encoding, and the buffer writes all happen outside the CS.

6. **Exact reservation via a count pass, not a scratch buffer.** `frameLen` needs
   the total ULEB arg length *before* the atomic reservation. Rather than encode
   the args into a `MAX_ARGS*5` (40 B) stack buffer to measure them, a `va_copy`
   count pass sums `ulebLen` per arg; the encode pass then writes each arg
   straight into the reserved ring through a 5-byte scratch. Costs a second cheap
   ULEB walk, saves 35 B of stack (see Stack below). Wire output is identical.

7. **Drop/tearing accounting.** `recCount` (committed records) lets the host
   compute exact drops = ΔrecCount − Δdecoded − inflight; `overflow` counts LINEAR
   full drops; torn frames show up as COBS/id/monotonicity failures. The host
   never mistakes falling behind for corruption.

## Measured overhead (CC2745, Cortex-M33, ticlang, this SDK)

### Flash (loaded)

One-time sink code, from `nm -S` on a linked image:

| symbol | bytes |
|---|---|
| `LogSinkBuf_printf` | 282 |
| `LogSinkBuf_bufDepInjection` | 254 |
| `LogSinkBuf_cobsFeed` | 78 |
| `LogSinkBuf_printfSingleton{,0,1,2,3}` | 28 + 4x32 = 156 |
| `LogSinkBuf_cobsPut`/`uleb`/`ulebLen`/`cobsFinish` | 28 + 30 + 16 + 14 |
| **total sink code** | **~860 B** |

The `printfDepInjection*` variants are only linked if an app uses the
multi-instance delegates; the SysConfig singleton app dead-strips them.

Per call site: ~18-24 B of loaded code (three `ldr` of handle/header/id-slot from
the literal pool, the arg loads, and the `bl`).

### Flash/RAM metadata (NOT loaded -- free)

`.log_data` (format string + file/line/level/module) and `.log_ptr` (the id slot)
link at non-loaded addresses (`0x90000000` / `0x94000008`, COPY sections). Measured
~278 B `.log_data` + 4 B `.log_ptr` **per site**, costing **0 flash and 0 RAM** in
the image. The real site ceiling is the `.log_data` region size, not flash.

### RAM

- Instance struct `LogSinkBuf_Instance`: **28 B** (buffer ptr, size, wrReserve,
  lastTs, recCount, overflow, bufType).
- The ring buffer: **`bufSize` bytes**, SysConfig-configurable (1024 default).
- Per record in the ring: **~5-9 B** (0-arg = 5 B: id 2 + delta 1 + COBS 1 +
  delimiter 1; each arg adds its 1-5 B ULEB).

### Stack

`LogSinkBuf_printf` frame = `sub sp,#44` locals + 8 pushed regs = **76 B**. Peak
depth for a log call adds the small `cobsInit/Feed/Finish`, `TimestampP`, and
`HwiP` frames -> **~120 B peak**. (This is why a 304-byte FreeRTOS idle stack
overflowed when Power/RCL logged from the idle/standby context; the fix is to size
the logging task's stack for a ~120 B log call, not to shrink the sink further.)

### Critical section

~27 instructions between `HwiP_disable` and `HwiP_restore` (delta clamp, one
`ulebLen`, the fit checks, the reservation) -> ~30-40 cycles, **< 1 µs** at 48 MHz
with interrupts masked.

### Cycles to enter a record (hardware, DWT CYCCNT, min = uninterrupted)

Measured at `LogSinkBuf_printf` entry->return on the running target:

| numArgs | cycles | ~µs @ 48 MHz |
|---|---|---|
| 0 | 354 | 7.4 |
| 1 | 437 | 9.1 |
| 2 | 538 | 11.2 |
| 3 | 786 | 16.4 |

Per-arg cost scales with the arg's **ULEB length** (1-5 bytes), not just the
count: the 3-arg sample logs large pointers (5-byte ULEB each), so it costs more
than a linear extrapolation of small-valued args (~80-100 cyc/arg). `Log_buf`
adds the payload memcpy-via-COBS on top of a similar fixed base.

## Can any argument be dropped?

The `Log_printfN_fxn` delegate is `(handle, header, headerPtr, ...args)`. Since the
16-bit id shrink, sinks use only `headerPtr` (the id slot) and `(void)header` the
32-bit metadata pointer -- so `header` is **dead**, materialized as a `ldr r1,[pc]`
+ a 4-byte literal (~6 B) at every call site.

It still **cannot be dropped**: `header` is part of the framework delegate ABI, and
every prebuilt logging library (RCL, the BLE controller/host, driverlib) bakes the
current `Log_printf` macro -- which passes `header` -- into its call sites. Changing
the delegate signature would ABI-break all of them (the same failure class that a
stale prebuilt `log_*.a` caused: a caller passing an arg the callee no longer
expects). Dropping `header` is therefore a whole-SDK rebuild, out of scope for the
sink. Internally, `header` is a free register pass-through into `LogSinkBuf_printf`,
so removing it there saves nothing. **Net: no argument is safely droppable.**

## Consequences

- The sink is a good fit for high-rate logging: sub-µs CS, ~350 cyc base cost, and
  ~5-9 B/record so a KB-scale ring holds a useful window.
- Falling behind is safe and observable (`drops`/`torn`), never corruption.
- The host decode (~2 MB/s pure Python) far outruns any SWD read backend, so the
  probe is the only throughput ceiling -- see the buf transport README.
