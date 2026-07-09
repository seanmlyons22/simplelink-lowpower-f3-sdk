# rftrace decode architecture: where work happens, and why the split is deliberate

This note explains how the RF-core trace path is layered across the **Rust `tracedecode`
binary** (`tools/rftrace-decode`) and the **Python tilogger framework** (`tools/log/tiutils`),
and — importantly — why the apparent overlap between them (two printf engines, two dbgid
resolvers, two pcap writers) is intentional and should **not** be "cleaned up" by merging.

## The two entry points

`tracedecode` is not just a tilogger backend. It has two independent callers:

1. **Standalone LA / Saleae workflow — no tilogger.**
   `tracedecode replay cap.sal --dbgid pbe.h stdout` (or `pcap --out f.pcap`, or the
   `wireshark` extcap). Here `tracedecode` is the whole tool: it deframes the sampled pin,
   assembles packets, resolves dbgids, formats the printf text, and writes stdout/pcap itself.
   Nothing Python is involved.

2. **tilogger framework — the `rftrace` transport.**
   `tilogger rftrace --port /dev/ttyACM0 --elf app.out wireshark` runs `tracedecode` as a
   subprocess and adapts its output into the tilogger pipeline so RF-core logs share the same
   aliasing, columns, and Wireshark/stdout sinks as the `itm`/`uart` transports.

Because of entry point (1), the Rust "upper stack" (dbgid resolution + printf + pcap) is
**load-bearing on its own** and cannot be removed.

## Data flow for the tilogger `rftrace` path

```
Pico USB (RFT1 words)  ─┐
Saleae .sal / raw / sigrok ─┼─►  tracedecode (Rust)
                             │      deframe → PacketAssembler → resolve(dbgid) → format_c → PcapSink
                             │                                                              │ classic-pcap DLT_USER0
                             ▼                                                              ▼  (text already final)
                      rftrace_transport.py  ──parse_pcap_records──►  LogPacket(opcode=REPLAY_FILE)
                             │
                             ▼
                      Logger.log()  ── REPLAY_FILE ⇒ skips re-resolve/re-format ──►  outputs (wireshark / stdout)
```

The key line is `opcode=REPLAY_FILE`: `Logger.log` only runs the Python resolver/formatter
(`format_dobby_packet`) for Log.h opcodes `< RESERVED_OPCODES` **except** `REPLAY_FILE`
(`core/tilogger/logger.py`). So the pre-formatted Rust text passes through untouched — there is
**no double decode or double format at runtime**. The Python side is a thin adapter, not a second
decoder.

## Why the duplication is justified (do not merge)

| Concern | Rust `tracedecode` | Python tilogger | Can they be one? |
|---|---|---|---|
| dbgid resolution | `dbgid.rs` parses `DBG_DEF(...)` LRF headers (pbe/rfe/mce) **and** ELF-extracted defs | `tracedb.py` resolves ELF **DWARF** `.log_ptr` slots (`logIndexDB`) — it does **not** parse `DBG_DEF` files | No. The two consume different metadata formats. LRF `DBG_DEF` parsing lives only in Rust. |
| printf (`%d/%x/%s/...`) | `record.rs::format_c` | `logger.py::format_c` / `format_dobby_packet` | Merging means either the standalone LA tool loses formatting, or high-rate LRF formatting moves into slower Python. |
| pcap emit | `output.rs::PcapSink` (its own stdout/pcap/extcap outputs) | `wireshark/main.py::WiresharkOutput` (tilogger DLT_USER0 columns + FIFO) | Two consumers: the Rust pcap is the Rust→Python IPC / standalone output; the Python pcap carries the tilogger column layout for the live GUI. |

Merging the printf/dbgid/pcap logic across the two languages would **add** code (re-implement LRF
`DBG_DEF` + arg-width/`par_cnt` semantics in Python), slow the high-rate LRF path, and force a
re-run of the HIL acceptance — to remove duplication that costs nothing at runtime. Net negative.

## The pcap seam is the clean boundary

The classic-pcap DLT_USER0 stream is the contract between Rust and Python. It is:
- self-describing (global header + per-record `ts`/`caplen`/payload),
- already the exact shape Wireshark wants, and
- versioned by the RFT1 wire (upstream) and the DLT_USER0 dissector (downstream).

`rftrace_transport.parse_pcap_records` is the whole adapter — read a record, split the `||`
columns, emit a `LogPacket`. That thinness is the point: the seam stays at pcap, not at a shared
in-process type, precisely so the two stacks can evolve independently.

## What each layer owns (contributor cheat-sheet)

- **New tracer wire feature** (frame layout, CRC, RFT1 control frames): Rust —
  `rftrace-decode` (and the `rftrace-wire` crate in the Pico repo it is checked against).
- **New printf specifier / dbgid field / LRF header format**: Rust `record.rs` / `dbgid.rs`.
- **New live source** (another receiver): usually the Python `rftrace_transport` (spawn/adapt),
  unless it changes the wire (then Rust too).
- **New sink / column / alias behavior for all transports**: Python tilogger outputs.
- **Anything that must also work with `itm`/`uart`**: Python — that is the shared framework.

Bottom line: the Rust/Python split is along a capability boundary (fast standalone deframe+resolve
vs. multi-source framework integration), not an accident. Keep the seam at pcap.
