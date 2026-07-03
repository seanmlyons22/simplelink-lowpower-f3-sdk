# rftrace-decode

Logic-analyzer backend for the CC-series **RF-core (LRFDTRC) trace sink**
(`source/ti/log/LogSinkTraceLPF3`). Takes LA samples of the `rfctrc_out` pin →
deframes → decodes the tracer packet protocol → resolves dbgids → emits logs to
**Wireshark and stdout, identically to the ITM/UART `tilogger` path**.

Full spec: **`../../trace-pipeline-architecture.md.mkd`, Part B (§11–§18)**.
Wire ground truth: `rf_tracer_spec.md`. Metadata: `elf2dbgid.py` `DBG_DEF` files.

## Pipeline

```
samples(u8) → deframe → Word(10b) → classify → WordKind
            → PacketAssembler (3× per-channel SM + CRC-5 + timestamp) → DecodedPacket
            → resolve(&DbgIdDb)  → LogRecord → Output (stdout / pcap DLT_USER0=147, || payload)
```

## Status

**Complete, std-only, `cargo test` fully green** (13 pass, 0 ignored), including the
golden end-to-end gate: `tx_burst_example.sal` decodes to the exact app+pbe record
sequence of `tx_burst_example_decoded.txt` (23/23, 0 framing errors, 0 CRC errors).

## Performance (live capture)

`decode` **streams** — reads samples in 1 MiB chunks with a one-frame carry, emits records
live, and runs **endlessly** with bounded memory. It never buffers the whole capture, so a
live `sigrok-cli … | tracedecode decode --raw -` keeps up with a 500 MS/s pipe forever.

Measured single-core (300 MB raw, `/usr/bin/time -v`):

| Input                         | Throughput  | vs 500 MS/s line | Peak RSS |
|-------------------------------|-------------|------------------|----------|
| idle / static line (scan)     | ~1070 MS/s  | 2.1×             | 5.2 MB   |
| dense packets (worst case)    | ~790 MS/s   | 1.6×             | 5.4 MB   |

Real traffic is mostly idle NOPs, so it sits near the top row. Memory is O(chunk), not
O(capture): a 300 MB input decodes in ~5 MB RSS. `stream_decode_crosses_chunk_boundaries`
tests that frames straddling a chunk boundary are neither dropped nor duplicated.
`.sal` replay stays batch (finite, edge-compressed — 11.5 G samples in ~1.2 s).

Empirically resolved against the golden capture:
- **CRC-5**: the MSB-first convention (`crc5::Crc5`, poly 0x05, init 0x1F) is correct —
  28/28 real packets validate. The reflected USB form does not match the wire.
- **`.sal` `digital-N.bin`**: not a flat f64 transition array. Real layout: 0x33-byte
  header (magic, version, capture start time, block count), then per-block
  `[begin, end, nsamples, rate, 1, payload_len]` u64s + varint-encoded run lengths
  (first byte: 6 data bits + bit6 continue; later bytes: 7 bits + bit7 continue;
  run = value+1 samples) + a jump table whose entry 0 carries the block start level.
  See `sample::decode_saleae_runs`.
- **Polarity**: on the real wire the start bit is HIGH (Saleae "inverted" UART sense).
  `deframe` auto-detects start-bit polarity (minority level), so both the synth
  convention and the real capture decode with the same defaults.
- **CC2745 timestamps** are 0.25 µs ticks → pass `--divide-time-by-2` to match wall time.

## Run

```
cargo test
cargo run -- replay tx_burst_example.sal --channel 4 --divide-time-by-2 \
    --dbgid app_dbgid.h --dbgid pbe_dbgid.h stdout
cargo run -- replay ... pcap --out trace.pcap        # DLT_USER0=147, tilogger payload
cargo run -- tail   tx_burst_example.sal --last 20 --dbgid app_dbgid.h
cargo run -- retain tx_burst_example.sal --dbgid app_dbgid.h --out ring   # dumpcap ring
cargo run -- wireshark --extcap-interfaces            # extcap; --capture --fifo <path>
cargo run -- synth  --dbgid app_dbgid.h --out synth.raw
cargo run -- decode --raw synth.raw --channel 4 --dbgid app_dbgid.h stdout
```

## Wireshark setup

The decoder emits the **same pcap the ITM/UART `tilogger` path emits** — DLT_USER0 (147)
with the `||`-delimited payload — so it reuses `tools/log/tiutils/streams/wireshark/`
`tilogger_dissector.lua` unchanged. Two one-time steps:

1. **Dissector + DLT_USER mapping.** Copy `tilogger_dissector.lua` into your Wireshark
   plugins dir (Help > About > Folders > "Personal Lua Plugins"), and map DLT 147 to it:
   Edit > Preferences > Protocols > DLT_USER > Edit… > add `DLT=147`, payload `tilogger`.
   (tilogger's own launcher passes this as `-o uat:user_dlts:...`; the extcap path can't, so
   set it once in preferences.)
2. Build the binary: `cargo build --release` → `target/release/tracedecode`.

### Launch from Wireshark (extcap — replay a `.sal`)

```
ln -s "$PWD/extcap/rftrace-decode" ~/.config/wireshark/extcap/rftrace-decode
```

`extcap/rftrace-decode` is a thin wrapper over `tracedecode wireshark`. Restart Wireshark;
"RF-core trace (LRFDTRC) decoder" appears as an interface. Its gear icon exposes the `.sal`
file, dbgid file(s), channel, and the `--divide-time-by-2` flag; start capture to stream the
decoded logs in. (Set `TRACEDECODE=/path/to/tracedecode` if the binary isn't on `PATH`.)

### Live from sigrok (logic analyzer → decoder → Wireshark)

```
scripts/sigrok-to-wireshark.sh --dbgid app_dbgid.h --dbgid pbe_dbgid.h \
    --driver saleae-logic-pro-16 --channel 4 --samplerate 500M --divide-time-by-2
```

One pipe: `sigrok-cli -O binary | tracedecode decode --raw - … pcap --out - | wireshark -k -i -`.
Runs **endlessly** (omit `--samples`/`--time`); the decoder streams with bounded memory.

## Launcher scripts

| Script | End-to-end flow |
|--------|-----------------|
| `scripts/tilogger-rftrace.sh <rftrace args…> <output>` | Builds the binary if needed, sets `$TRACEDECODE`, runs `tilogger rftrace …` (the integrated path). |
| `scripts/sigrok-to-wireshark.sh --dbgid … [opts]` | Standalone `sigrok-cli \| tracedecode \| wireshark` pipe (no Python). |
| `extcap/rftrace-decode` | Wireshark-launched extcap wrapper (symlink into Wireshark's extcap dir). |

## Integration with `tilogger` (primary UX)

This decoder is wired into the existing `tilogger` tool as a first-class **transport**,
so it swaps in for `itm`/`uart` with the same command shape and reuses every tilogger
output (`stdout`, `wireshark`, `to-replayfile`) unchanged:

```
tilogger rftrace --sal cap.sal --dbgid app_dbgid.h --dbgid pbe_dbgid.h \
    --channel 4 --divide-time-by-2 stdout
tilogger rftrace --sal cap.sal --dbgid app_dbgid.h --divide-time-by-2 wireshark --start
tilogger rftrace --sigrok "--driver saleae-logic-pro-16 --config samplerate=500M \
    -C D4" --dbgid app_dbgid.h --channel 0 --divide-time-by-2 stdout   # endless live
```

The transport (`tools/log/tiutils/streams/rftrace/`) runs this `tracedecode` binary with
`pcap --out -` and adapts its `||` records into tilogger `LogPacket`s (the `from-replayfile`
pattern), so metadata resolution and formatting stay here and presentation stays shared with
ITM/UART. Point it at the binary via `--tracedecode`, `$TRACEDECODE`, or `PATH`. See
`tools/log/tiutils/README.md` → "RF-core Trace (rftrace) Transport".

The same change made tilogger's **Wireshark output work on Linux** (`streams/wireshark`): the
win32 named-pipe path was guarded and a FIFO path added, so `wireshark --start` auto-launches
and configures Wireshark on Linux and Windows alike.

## Standalone use (no tilogger)

The `replay`/`decode`/`tail`/`retain`/`wireshark` subcommands, the `extcap/` wrapper, and
`scripts/sigrok-to-wireshark.sh` above run the decoder directly without the Python tool —
same pcap DLT_USER0 stream, same `tilogger_dissector.lua`. Per architecture ADR-017 the
decode core is a standalone native backend; the tilogger transport is a thin adapter on top,
not a rewrite. The `cli_emits_tilogger_pcap_over_stdout` test asserts the binary's pcap
contract (DLT 147 + the 8 dissector columns).
