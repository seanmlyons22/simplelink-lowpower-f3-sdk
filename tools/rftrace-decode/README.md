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

**Complete, std-only, `cargo test` fully green** (11 pass, 0 ignored), including the
golden end-to-end gate: `tx_burst_example.sal` decodes to the exact app+pbe record
sequence of `tx_burst_example_decoded.txt` (23/23, 0 framing errors, 0 CRC errors).

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
