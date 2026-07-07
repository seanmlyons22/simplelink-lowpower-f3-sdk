# rftrace-decode

Logic-analyzer backend for the CC-series RF-core (LRFDTRC) trace sink
(`source/ti/log/LogSinkTraceLPF3`). Takes LA samples of the trace pin, deframes
the 12-bit tracer line, decodes the packet protocol, resolves dbgids, and emits
logs to Wireshark and stdout identically to the ITM/UART `tilogger` path.

The self-contained pipeline description (wire format, module map, decision log)
is `trace-pipeline-architecture.md.mkd` at the repo root. Metadata comes from
`elf2dbgid` `DBG_DEF` files; the decoder never parses an ELF.

## Pipeline

```
samples(u8) -> deframe -> Word(10b) -> classify -> WordKind
            -> PacketAssembler (3x per-channel SM + CRC-5 + timestamp) -> DecodedPacket
            -> resolve(DbgIdDb) -> LogRecord -> Output (stdout / pcap DLT_USER0=147, || payload)
```

## Status

Complete, std-only (zero dependencies), `cargo test` fully green: ~50 module unit
tests, integration tests including the golden end-to-end gate (`tx_burst_example.sal`
decodes to the exact app+pbe record sequence of the oracle, zero framing and CRC
errors), and a CLI suite covering every subcommand and flag of the built binary.

## Performance (live capture)

`decode` streams: it reads samples in 1 MiB chunks with a one-frame carry, emits
records live, and runs endlessly in bounded memory, so a live
`sigrok-cli ... | tracedecode decode --raw -` keeps up with a 500 MS/s pipe.

Measured single-core on 5 GB synthetic captures (`scripts/bench_decode.sh`, exact
record count and zero health counters asserted on every run):

| Input                                | Throughput | vs 500 MS/s line | Peak RSS |
|--------------------------------------|------------|------------------|----------|
| sparse (99% NOP idle, realistic)     | ~1000 MS/s | 2.0x             | 4.4 MB   |
| dense (back-to-back, 3 channels)     | ~610 MS/s  | 1.2x             | 4.5 MB   |

Memory is O(chunk), not O(capture). `.sal` replay stays batch but edge-compressed:
the 11.5 G-sample golden capture decodes in about 1.2 s.

The decoder core stays in Rust by measurement, not preference: a byte-identical
Python port (`scripts/pydecode.py`, pure and NumPy variants) tops out at 369 MS/s
on the easiest profile - under the 500 MS/s live bar. Numbers and method are in
the architecture doc's decision record; the port is pinned byte-identical to this
decoder by `tests/cli.rs` so the decision stays reproducible.

Empirically resolved against the golden capture:

- CRC-5 is the MSB-first convention (poly 0x05, init 0x1F): 28/28 real packets
  validate. The textbook reflected CRC-5/USB does not match the wire.
- `.sal` `digital-N.bin` is a block/varint run-length format, not raw samples;
  see `sample::decode_saleae_runs` for the reverse-engineered layout.
- On the real wire the start bit is high (Saleae "inverted" UART sense). The
  deframer auto-detects start-bit polarity (minority level), so real captures and
  synthetic ones decode with the same defaults.
- CC2745 tracer ticks are 0.25 us: pass `--divide-time-by-2` to read wall-clock.

## Run

```
cargo test                              # unit + integration + CLI suites
cargo run -- replay capture.sal --channel 4 --divide-time-by-2 \
    --dbgid app_dbgid.h --dbgid pbe_dbgid.h stdout
cargo run -- replay ... pcap --out trace.pcap   # DLT_USER0=147, tilogger payload
cargo run -- tail   capture.sal --last 20 --dbgid app_dbgid.h
cargo run -- retain capture.sal --dbgid app_dbgid.h --out ring  # dumpcap ring
cargo run -- wireshark --extcap-interfaces      # extcap; --capture --fifo <path>
cargo run -- synth  --dbgid app_dbgid.h --out synth.raw [--repeat N] [--idle-frames K]
cargo run -- decode --raw synth.raw --channel 4 --dbgid app_dbgid.h stdout
```

Gated extras:

```
RFTRACE_PY_GOLDEN=1 cargo test pydecode_matches_rust_on_golden_sal   # ~35 s
SIZE_GB=5 DECODE=target/release/tracedecode scripts/bench_decode.sh dense
SIZE_GB=5 DECODE=target/release/tracedecode scripts/bench_decode.sh sparse
```

## Wireshark setup

The decoder emits the same pcap the ITM/UART `tilogger` path emits - DLT_USER0
(147) with the `||`-delimited payload - so it reuses
`tools/log/tiutils/streams/wireshark/tilogger_dissector.lua` unchanged. Two
one-time steps:

1. Dissector + DLT_USER mapping. Copy `tilogger_dissector.lua` into your
   Wireshark plugins dir (Help > About > Folders > "Personal Lua Plugins") and
   map DLT 147 to it: Edit > Preferences > Protocols > DLT_USER > Edit, add
   DLT=147, payload `tilogger`. (tilogger's own launcher passes this as
   `-o uat:user_dlts:...`; the extcap path cannot, so set it once in preferences.)
2. Put the binary on PATH: `cargo install --path .` (installs
   `~/.cargo/bin/tracedecode`). Or build with `cargo build --release` and set
   `$TRACEDECODE` / pass `--tracedecode <path>`.

### Launch from Wireshark (extcap - replay a .sal)

```
ln -s "$PWD/extcap/rftrace-decode" ~/.config/wireshark/extcap/rftrace-decode
```

`extcap/rftrace-decode` is a thin wrapper over `tracedecode wireshark`. Restart
Wireshark; "RF-core trace (LRFDTRC) decoder" appears as an interface. Its gear
icon exposes the `.sal` file, dbgid file(s), channel, and `--divide-time-by-2`;
start capture to stream the decoded logs in. (Set `TRACEDECODE=/path/to/binary`
if it is not on PATH.)

### Live from sigrok (logic analyzer -> decoder -> Wireshark)

```
scripts/sigrok-to-wireshark.sh --dbgid app_dbgid.h --dbgid pbe_dbgid.h \
    --driver saleae-logic-pro-16 --channel 4 --samplerate 500M --divide-time-by-2
```

One pipe: `sigrok-cli -O binary | tracedecode decode --raw - ... pcap --out - |
wireshark -k -i -`. Runs endlessly (omit `--samples`/`--time`); the decoder
streams with bounded memory.

## Launcher scripts

| Script | End-to-end flow |
|--------|-----------------|
| `scripts/tilogger-rftrace.sh <rftrace args...> <output>` | Builds the binary if needed, sets `$TRACEDECODE`, runs `tilogger rftrace ...` (the integrated path). |
| `scripts/sigrok-to-wireshark.sh --dbgid ... [opts]` | Standalone `sigrok-cli | tracedecode | wireshark` pipe (no Python). |
| `scripts/bench_decode.sh {dense|sparse}` | Throughput/RSS benchmark with correctness asserted (see architecture doc). |
| `scripts/pydecode.py` | Byte-identical Python port of the decode path; keeps the language decision reproducible. |
| `extcap/rftrace-decode` | Wireshark-launched extcap wrapper (symlink into Wireshark's extcap dir). |

## Integration with `tilogger` (primary UX)

This decoder is wired into the existing `tilogger` tool as a first-class
transport, so it swaps in for `itm`/`uart` with the same command shape and reuses
every tilogger output (`stdout`, `wireshark`, `to-replayfile`) unchanged:

```
tilogger rftrace --sal cap.sal --elf app.out \
    --channel 4 --divide-time-by-2 stdout                       # metadata from the ELF
tilogger rftrace --sal cap.sal --elf app.out --dbgid pbe_dbgid.h \
    --divide-time-by-2 wireshark --start                        # + modem/LRF headers
tilogger rftrace --sigrok "--driver saleae-logic-pro-16 --config samplerate=500M \
    -C D4" --elf app.out --channel 0 --divide-time-by-2 stdout  # endless live
tilogger rftrace --logic2 --duration 2 --loop --elf app.out \
    --channel 7 --divide-time-by-2 wireshark --start            # Logic 2 automation
```

The transport (`tools/log/tiutils/streams/rftrace/`) runs this `tracedecode`
binary with `pcap --out -` and adapts its `||` records into tilogger `LogPacket`s
(the from-replayfile pattern), so formatting and presentation stay shared with
ITM/UART. `--elf <app.out>` resolves all CPU-side logs straight from the binary -
the transport reads the ELF's dbgid table via tilogger's own ELF parser
(`elf_dbgid.py`, verified against `elf2dbgid` output); extra `--dbgid` headers add
modem/LRF tables (pbe/rfe/mce). The decoder itself still only reads `DBG_DEF`
files and never parses an ELF; the transport bridges the ELF to it.

Its test suite (`streams/rftrace/tests/`, pytest) covers every CLI option, the
pcap adapter, and the whole `--logic2` capture loop against a faked Logic 2
automation client - no hardware in CI. One hardware-in-the-loop check exists and
is gated: `RFTRACE_HIL=1 pytest tests/ -k hil` (needs a Saleae attached, Logic 2
running with automation enabled, `TRACEDECODE`, and `RFTRACE_DBGID`).

The same change made tilogger's Wireshark output work on Linux
(`streams/wireshark`): the win32 named-pipe path was guarded and a FIFO path
added, so `wireshark --start` auto-launches and configures Wireshark on Linux and
Windows alike.

## Standalone use (no tilogger)

The `replay`/`decode`/`tail`/`retain`/`wireshark` subcommands, the `extcap/`
wrapper, and `scripts/sigrok-to-wireshark.sh` run the decoder directly without
the Python tool - same pcap DLT_USER0 stream, same `tilogger_dissector.lua`. The
decode core is a standalone native backend; the tilogger transport is a thin
adapter on top, not a rewrite. `cli_emits_tilogger_pcap_over_stdout` asserts the
binary's pcap contract (DLT 147 + the 8 dissector columns).
